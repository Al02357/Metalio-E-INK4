#include "monitor_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <wifi_station.h>

#include "battery_log.h"
#include "board.h"
#include "ecs_power.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "Monitor";
constexpr const char* kScreenName = "monitor";
constexpr int kPad = 16;
constexpr int kChartH = 150;

struct Srv {
    bool ok = false;
    std::string err;
    double load1 = 0;
    int cpus = 0, mem_total = 0, mem_avail = 0, uptime_s = 0, idle_min = 0, autostop_in = -1, autostop_idle = 60;
    int req = 0, user_req = 0, users = 0;
    double disk_free = 0, disk_total = 0;
    bool autostop = false;
};

struct Data {
    bool ready = false;
    bool bat_ok = false;
    BatterySample bat;
    std::vector<BatterySample> hist;
    bool sd_ok = false;
    uint64_t sd_total = 0, sd_free = 0;
    bool wifi = false;
    int rssi = 0;
    std::string ssid;
    size_t heap_int = 0, heap_ps = 0;
    Srv srv;
    bool has_key = false;
    std::string ecs, ecs_err;
};

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* msg = nullptr;
    lv_obj_t* stop_btn = nullptr;
    const lv_font_t* f = nullptr;
    const lv_font_t* fb = nullptr;
    int top = 0;
    bool stop_armed = false;
    lv_timer_t* disarm = nullptr;
    std::vector<std::vector<lv_point_precise_t>> lines;  // lv_line 只存指针，点数组要活到下次重建
};

std::mutex s_mu;
Data s_data;
std::string s_msg;
int s_range_days = 1;
std::atomic<bool> s_active{false};
std::atomic<bool> s_busy{false};
Ui s_ui;

// ---------------------------------------------------------------- 后台取数

bool ServerReq(const char* method, const std::string& path, std::string& body, std::string& err) {
    FILE* f = std::fopen(SD_APP_ROOT "/weread.json", "rb");
    if (f == nullptr) {
        err = "未配置服务器";
        return false;
    }
    std::string text(2048, '\0');
    text.resize(std::fread(&text[0], 1, text.size(), f));
    std::fclose(f);
    std::string server, token;
    if (cJSON* r = cJSON_Parse(text.c_str())) {
        const cJSON* sv = cJSON_GetObjectItemCaseSensitive(r, "server");
        const cJSON* tk = cJSON_GetObjectItemCaseSensitive(r, "token");
        if (cJSON_IsString(sv)) server = sv->valuestring;
        if (cJSON_IsString(tk)) token = tk->valuestring;
        cJSON_Delete(r);
    }
    while (!server.empty() && server.back() == '/') server.pop_back();
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http || server.empty()) {
        err = "无网络";
        return false;
    }
    http->SetTimeout(6000);
    http->SetHeader("X-Token", token);
    if (std::strcmp(method, "POST") == 0) http->SetContent(std::string());
    if (!http->Open(method, server + path)) {
        err = "连不上";
        return false;
    }
    const int st = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();
    if (st != 200) {
        err = "HTTP " + std::to_string(st);
        return false;
    }
    return true;
}

int JI(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
double JD(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

void PostRender();

void Collect(Data& d) {
    d.bat_ok = BatteryReadNow(d.bat);
    d.hist = BatteryLog_Read(static_cast<int64_t>(time(nullptr)) - 7 * 86400);
    d.sd_ok = esp_vfs_fat_info("/sdcard", &d.sd_total, &d.sd_free) == ESP_OK;
    auto& ws = WifiStation::GetInstance();
    d.wifi = ws.IsConnected();
    if (d.wifi) {
        d.rssi = ws.GetRssi();
        d.ssid = ws.GetSsid();
    }
    d.heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    d.heap_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // 本机数据先显示，不等联网
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_data.bat_ok = d.bat_ok;
        s_data.bat = d.bat;
        s_data.hist = d.hist;
        s_data.sd_ok = d.sd_ok;
        s_data.sd_total = d.sd_total;
        s_data.sd_free = d.sd_free;
        s_data.heap_int = d.heap_int;
        s_data.heap_ps = d.heap_ps;
        s_data.ready = true;
        s_msg = "正在连网查云服务器…";
    }
    PostRender();
    auto& board = Board::GetInstance();
    if (!board.IsNetworkReady()) board.EnsureNetworkReady(15000);
    d.wifi = ws.IsConnected();
    if (d.wifi) {
        d.rssi = ws.GetRssi();
        d.ssid = ws.GetSsid();
    }
    std::string body;
    d.srv = Srv{};
    // 先问阿里云：服务器关着就不用再等服务超时
    d.has_key = ecs::HasKey();
    d.ecs_err.clear();
    d.ecs = d.has_key ? ecs::Status(&d.ecs_err) : "";
    const bool skip_srv = d.has_key && !d.ecs.empty() && d.ecs != "Running";
    if (skip_srv) {
        d.srv.err = std::string("服务器") + ecs::StatusZh(d.ecs);
    } else if (ServerReq("GET", "/api/sysinfo", body, d.srv.err)) {
        if (cJSON* r = cJSON_Parse(body.c_str())) {
            d.srv.ok = true;
            d.srv.load1 = JD(r, "load1");
            d.srv.cpus = JI(r, "cpus");
            d.srv.mem_total = JI(r, "mem_total");
            d.srv.mem_avail = JI(r, "mem_avail");
            d.srv.disk_free = JD(r, "disk_free_gb");
            d.srv.disk_total = JD(r, "disk_total_gb");
            d.srv.uptime_s = JI(r, "uptime_s");
            d.srv.idle_min = JI(r, "idle_min");
            d.srv.autostop = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "autostop"));
            d.srv.autostop_in = JI(r, "autostop_in_min");
            d.srv.autostop_idle = JI(r, "autostop_idle_min");
            d.srv.req = JI(r, "req_today");
            d.srv.user_req = JI(r, "user_req_today");
            d.srv.users = JI(r, "users");
            cJSON_Delete(r);
        }
        if (!d.has_key && ecs::SyncKeyFromServer(false)) {
            d.has_key = true;
            d.ecs = ecs::Status(&d.ecs_err);
        }
    }
    d.ready = true;
}

void PostRender();

void SetMsg(const std::string& m) {
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_msg = m;
    }
    PostRender();
}

void JobRefresh(int);
using JobFn = void (*)(int);

// 正在刷新时点了开关机等：先记下，当前任务结束后马上执行
JobFn s_pending_fn = nullptr;
int s_pending_arg = 0;

void RunTask(JobFn fn, int arg) {
    if (s_busy.exchange(true)) {
        if (fn != nullptr && fn != JobRefresh) {
            std::lock_guard<std::mutex> lock(s_mu);
            s_pending_fn = fn;
            s_pending_arg = arg;
            s_msg = "收到，等这次刷新完就执行…";
        }
        PostRender();
        return;
    }
    struct J {
        JobFn fn;
        int arg;
    };
    auto* j = new J{fn, arg};
    auto task = [](void* p) {
        auto* jj = static_cast<J*>(p);
        {
            PowerNeedHold hold(PowerNeed::OtaDownload);
            jj->fn(jj->arg);
            for (;;) {
                JobFn next = nullptr;
                int arg = 0;
                {
                    std::lock_guard<std::mutex> lock(s_mu);
                    next = s_pending_fn;
                    arg = s_pending_arg;
                    s_pending_fn = nullptr;
                }
                if (next == nullptr) break;
                next(arg);
            }
        }
        delete jj;
        s_busy.store(false);
        vTaskDeleteWithCaps(nullptr);
    };
    if (xTaskCreateWithCaps(task, "monitor_job", 10 * 1024, j, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        delete j;
        s_busy.store(false);
    }
}

void JobRefresh(int) {
    const int64_t t0 = esp_timer_get_time();
    SetMsg("正在刷新…");
    Data d;
    Collect(d);
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_data = std::move(d);
        if (s_msg.rfind("正在", 0) == 0 || s_msg.rfind("收到", 0) == 0) s_msg.clear();
    }
    ESP_LOGI(TAG, "refresh done %dms", static_cast<int>((esp_timer_get_time() - t0) / 1000));
    PostRender();
}

void JobPower(int on) {
    std::string err;
    const std::string st0 = ecs::Status(&err);
    if (on && st0 == "Running") {
        SetMsg("云服务器已经在运行");
        JobRefresh(0);
        return;
    }
    if (!on && st0 == "Stopped") {
        SetMsg("云服务器已经关机");
        JobRefresh(0);
        return;
    }
    if (on) {
        SetMsg("正在开机…");
        if (!ecs::Start(&err)) {
            SetMsg("开机失败：" + err);
            return;
        }
        for (int i = 0; i < 40; ++i) {
            vTaskDelay(pdMS_TO_TICKS(4000));
            std::string body, e2;
            if (ServerReq("GET", "/api/sysinfo", body, e2)) break;
            SetMsg("开机中，等服务启动…（" + std::to_string((i + 1) * 4) + " 秒）");
        }
    } else {
        SetMsg("正在关机…");
        if (!ecs::Stop(&err)) {
            SetMsg("关机失败：" + err);
            return;
        }
        for (int i = 0; i < 30; ++i) {
            vTaskDelay(pdMS_TO_TICKS(4000));
            const std::string st = ecs::Status(&err);
            if (st == "Stopped") break;
            SetMsg(std::string("关机中…（") + ecs::StatusZh(st) + "）");
        }
    }
    JobRefresh(0);
}

void JobAutostop(int on) {
    std::string body, err;
    SetMsg("正在设置自动关机…");
    if (!ServerReq("POST", std::string("/api/autostop?enabled=") + (on ? "1" : "0"), body, err)) {
        SetMsg("设置失败：" + err);
        return;
    }
    JobRefresh(0);
}

// ---------------------------------------------------------------- 界面

lv_obj_t* Box(lv_obj_t* p) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* p, const std::string& t, const lv_font_t* f = nullptr) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f ? f : s_ui.f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, t.c_str());
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

lv_obj_t* Btn(lv_obj_t* p, const char* t, int w, int h, lv_event_cb_t cb, intptr_t ud, bool inv = false) {
    lv_obj_t* b = Box(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, inv ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(b);
    lv_obj_t* l = Label(b, t);
    lv_obj_set_style_text_color(l, inv ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(ud));
    return b;
}

lv_obj_t* Rule(lv_obj_t* p, int y, int w = 2) {
    lv_obj_t* r = Box(p);
    lv_obj_set_size(r, LV_HOR_RES - 2 * kPad, w);
    lv_obj_set_pos(r, kPad, y);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    return r;
}

std::string Dur(int s) {
    char b[32];
    if (s >= 86400) std::snprintf(b, sizeof(b), "%d天%d小时", s / 86400, s % 86400 / 3600);
    else if (s >= 3600) std::snprintf(b, sizeof(b), "%d小时%d分", s / 3600, s % 3600 / 60);
    else std::snprintf(b, sizeof(b), "%d分钟", s / 60);
    return b;
}

/** 用最近一段未充电的记录估算掉电速度（%/小时）。 */
double DrainPerHour(const std::vector<BatterySample>& h) {
    if (h.size() < 2) return 0;
    const int64_t now = h.back().t;
    size_t i = h.size() - 1;
    while (i > 0 && !h[i - 1].chg && now - h[i - 1].t < 24 * 3600) --i;
    const BatterySample& a = h[i];
    const BatterySample& b = h.back();
    if (b.t - a.t < 2 * 3600 || a.level <= b.level) return 0;
    return (a.level - b.level) * 3600.0 / static_cast<double>(b.t - a.t);
}

void OnRange(lv_event_t* e);
void OnRefresh(lv_event_t*);
void OnPower(lv_event_t* e);
void OnAutostop(lv_event_t* e);

void BuildChart(lv_obj_t* parent, int y, const std::vector<BatterySample>& all) {
    const int x0 = kPad + 52, w = LV_HOR_RES - kPad - x0, h = kChartH;
    lv_obj_t* frame = Box(parent);
    lv_obj_set_pos(frame, x0, y);
    lv_obj_set_size(frame, w, h);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_t* mid = Box(frame);  // 50% 虚线
    lv_obj_set_size(mid, w - 4, 1);
    lv_obj_set_pos(mid, 0, 6 + (h - 20) / 2 - 2);
    lv_obj_set_style_border_side(mid, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(mid, 1, 0);
    lv_obj_set_style_border_color(mid, lv_color_black(), 0);
    lv_obj_set_style_border_opa(mid, LV_OPA_50, 0);
    for (int pct : {100, 50, 0}) {
        lv_obj_t* l = Label(parent, std::to_string(pct));
        lv_obj_set_width(l, 46);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(l, kPad, y + 6 + (100 - pct) * (h - 20) / 100 - 14);
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int64_t span = static_cast<int64_t>(s_range_days) * 86400;
    const int64_t t0 = now - span;
    lv_obj_t* lb = Label(parent, s_range_days == 1 ? "24小时前" : "7天前");
    lv_obj_set_pos(lb, x0, y + h + 2);
    lv_obj_t* rb = Label(parent, "现在");
    lv_obj_align_to(rb, frame, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);

    // 断档（关机 / 没记录）超过 40 分钟就断开画
    std::vector<lv_point_precise_t> cur;
    int64_t last_t = 0;
    auto flush = [&]() {
        if (cur.size() == 1) {  // 只有一个点：画一小段横线
            lv_point_precise_t q = cur[0];
            q.x -= 8;
            cur.push_back(q);
        }
        if (cur.size() >= 2) s_ui.lines.push_back(std::move(cur));
        cur.clear();
    };
    for (const auto& s : all) {
        if (s.t < t0) continue;
        if (last_t && s.t - last_t > 40 * 60) flush();
        lv_point_precise_t p;
        // 内缩 8px，100% / 最新的点不被边框盖住
        p.x = static_cast<lv_value_precise_t>((s.t - t0) * (w - 20) / span + 6);
        p.y = static_cast<lv_value_precise_t>((100 - s.level) * (h - 20) / 100 + 6);
        cur.push_back(p);
        last_t = s.t;
    }
    flush();
    for (auto& pts : s_ui.lines) {
        lv_obj_t* ln = lv_line_create(frame);
        lv_obj_set_style_line_width(ln, 4, 0);
        lv_obj_set_style_line_rounded(ln, true, 0);
        lv_obj_set_style_line_color(ln, lv_color_black(), 0);
        lv_line_set_points(ln, pts.data(), static_cast<uint32_t>(pts.size()));
    }
    if (s_ui.lines.empty()) {
        lv_obj_t* l = Label(frame, all.empty() ? "还没有记录（每 10 分钟记一次）" : "这段时间没有记录");
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 20);
    }
}

void Render() {
    if (!s_active.load() || s_ui.body == nullptr) return;
    Data d;
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        d = s_data;
        msg = s_msg;
    }
    lv_obj_clean(s_ui.body);
    s_ui.lines.clear();
    s_ui.stop_btn = nullptr;
    s_ui.stop_armed = false;
    lv_obj_t* b = s_ui.body;
    char buf[160];
    int y = 6;

    lv_obj_t* title = Label(b, "监控", s_ui.fb);
    lv_obj_set_pos(title, kPad, y + 4);
    lv_obj_t* rf = Btn(b, "刷新", 96, 44, OnRefresh, 0);
    lv_obj_set_pos(rf, LV_HOR_RES - kPad - 96, y);
    s_ui.msg = Label(b, msg);
    lv_obj_set_pos(s_ui.msg, kPad + 90, y + 10);
    y += 54;
    Rule(b, y);
    y += 10;

    // ---- 本机
    lv_obj_t* h1 = Label(b, "本机", s_ui.fb);
    lv_obj_set_pos(h1, kPad, y);
    lv_obj_t* r7 = Btn(b, "7天", 80, 40, OnRange, 7, s_range_days == 7);
    lv_obj_set_pos(r7, LV_HOR_RES - kPad - 80, y);
    lv_obj_t* r1 = Btn(b, "24小时", 110, 40, OnRange, 1, s_range_days == 1);
    lv_obj_set_pos(r1, LV_HOR_RES - kPad - 80 - 10 - 110, y);
    y += 50;
    if (d.bat_ok) {
        std::snprintf(buf, sizeof(buf), "电量 %d%%   %.2fV   %+dmA", d.bat.level, d.bat.mv / 1000.0, d.bat.ma);
        Label(b, buf);
        lv_obj_set_pos(lv_obj_get_child(b, -1), kPad, y);
        y += 32;
        std::string est;
        if (d.bat.chg) {
            est = "充电中";
        } else {
            const double dph = DrainPerHour(d.hist);
            if (dph > 0.05) {
                const double hrs = d.bat.level / dph;
                std::snprintf(buf, sizeof(buf), "每小时约掉 %.1f%%，还能用约 %s", dph,
                              hrs >= 48 ? (std::to_string(static_cast<int>(hrs / 24 + 0.5)) + " 天").c_str()
                                        : (std::to_string(static_cast<int>(hrs + 0.5)) + " 小时").c_str());
                est = buf;
            } else {
                est = "记录满 2 小时后估算还能用多久";
            }
        }
        lv_obj_t* l = Label(b, est);
        lv_obj_set_pos(l, kPad, y);
        y += 38;
    } else {
        lv_obj_set_pos(Label(b, d.ready ? "电量读取失败" : "读取中…"), kPad, y);
        y += 38;
    }
    BuildChart(b, y, d.hist);
    y += kChartH + 34;
    std::snprintf(buf, sizeof(buf), "已开机 %s   固件 %s", Dur(static_cast<int>(esp_timer_get_time() / 1000000)).c_str(),
                  esp_app_get_description()->version);
    lv_obj_set_pos(Label(b, buf), kPad, y);
    y += 32;
    std::snprintf(buf, sizeof(buf), "内存 %uKB / PSRAM %.1fMB", static_cast<unsigned>(d.heap_int / 1024),
                  d.heap_ps / 1048576.0);
    lv_obj_set_pos(Label(b, buf), kPad, y);
    y += 32;
    std::string net = d.wifi ? ("Wi-Fi " + std::to_string(d.rssi) + "dBm") : "Wi-Fi 未连接";
    if (d.sd_ok) {
        std::snprintf(buf, sizeof(buf), "SD 剩余 %.1f/%.1fGB   %s", d.sd_free / 1073741824.0, d.sd_total / 1073741824.0,
                      net.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "SD 未读到   %s", net.c_str());
    }
    lv_obj_set_pos(Label(b, buf), kPad, y);
    y += 40;
    Rule(b, y);
    y += 10;

    // ---- 云服务器
    lv_obj_t* h2 = Label(b, "云服务器", s_ui.fb);
    lv_obj_set_pos(h2, kPad, y);
    if (d.has_key) {
        s_ui.stop_btn = Btn(b, "关机", 90, 40, OnPower, 0);
        lv_obj_set_pos(s_ui.stop_btn, LV_HOR_RES - kPad - 90, y);
        lv_obj_t* on = Btn(b, "开机", 90, 40, OnPower, 1);
        lv_obj_set_pos(on, LV_HOR_RES - kPad - 90 - 10 - 90, y);
    }
    y += 50;
    std::string st;
    if (!d.ready) st = "读取中…";
    else if (!d.has_key) st = "状态 未知（还没拿到开关机 Key）";
    else if (!d.ecs.empty()) st = std::string("状态 ") + ecs::StatusZh(d.ecs);
    else st = "状态 查询失败：" + d.ecs_err;
    lv_obj_set_pos(Label(b, st), kPad, y);
    y += 32;
    if (d.srv.ok) {
        std::snprintf(buf, sizeof(buf), "CPU 负载 %.2f/%d核   内存 %d/%dMB", d.srv.load1, d.srv.cpus,
                      d.srv.mem_total - d.srv.mem_avail, d.srv.mem_total);
        lv_obj_set_pos(Label(b, buf), kPad, y);
        y += 32;
        std::snprintf(buf, sizeof(buf), "磁盘 剩余 %.0f/%.0fGB   已运行 %s", d.srv.disk_free, d.srv.disk_total,
                      Dur(d.srv.uptime_s).c_str());
        lv_obj_set_pos(Label(b, buf), kPad, y);
        y += 32;
        std::snprintf(buf, sizeof(buf), "今天请求 %d 次（使用 %d）  空闲 %d 分", d.srv.req, d.srv.user_req,
                      d.srv.idle_min);
        lv_obj_set_pos(Label(b, buf), kPad, y);
        y += 40;
        if (d.srv.autostop) {
            std::snprintf(buf, sizeof(buf), "空闲 %d 分钟自动关机%s", d.srv.autostop_idle,
                          d.srv.users > 0 ? "（有人登录，暂不关）"
                                          : ("，还剩 " + std::to_string(d.srv.autostop_in) + " 分钟").c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "自动关机 已关闭");
        }
        lv_obj_t* l = Label(b, buf);
        lv_obj_set_width(l, LV_HOR_RES - 2 * kPad - 110);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(l, kPad, y + 6);
        lv_obj_t* ab = Btn(b, d.srv.autostop ? "关闭" : "开启", 96, 44, OnAutostop, d.srv.autostop ? 0 : 1);
        lv_obj_set_pos(ab, LV_HOR_RES - kPad - 96, y);
    } else if (d.ready) {
        lv_obj_set_pos(Label(b, "服务连不上（" + d.srv.err + "）"), kPad, y);
    }
}

void RenderAsync(void*) { Render(); }
void PostRender() { ScreenLvAsync(RenderAsync); }

void OnRefresh(lv_event_t*) { RunTask(JobRefresh, 0); }

void OnRange(lv_event_t* e) {
    s_range_days = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    Render();
}

void Disarm(lv_timer_t*) {
    s_ui.disarm = nullptr;
    if (s_ui.stop_btn && s_ui.stop_armed) {
        s_ui.stop_armed = false;
        lv_label_set_text(lv_obj_get_child(s_ui.stop_btn, 0), "关机");
    }
}

void OnPower(lv_event_t* e) {
    const int on = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!on && !s_ui.stop_armed) {  // 关机要点两次
        s_ui.stop_armed = true;
        lv_obj_set_width(s_ui.stop_btn, 130);
        lv_obj_set_x(s_ui.stop_btn, LV_HOR_RES - kPad - 130);
        lv_label_set_text(lv_obj_get_child(s_ui.stop_btn, 0), "再点关机");
        s_ui.disarm = lv_timer_create(Disarm, 5000, nullptr);
        lv_timer_set_repeat_count(s_ui.disarm, 1);
        return;
    }
    SetMsg(on ? "正在开机…" : "正在关机…");
    RunTask(JobPower, on);
}

void OnAutostop(lv_event_t* e) {
    RunTask(JobAutostop, static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void OnLifecycle(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->SetStatusTitlePrefix(nullptr);
            disp->UpdateStatusBar(true);
        }
        Render();
        RunTask(JobRefresh, 0);
    } else if (lv_event_get_target(e) == s_ui.scr) {
        s_active.store(false);
        if (s_ui.disarm) lv_timer_delete(s_ui.disarm);
        s_ui.disarm = nullptr;
        s_ui.body = nullptr;  // 屏对象由导航删除；点数组留到下次 Create 再清
        s_ui.stop_btn = nullptr;
    }
}

}  // namespace

lv_obj_t* MonitorScreen::Create() {
    s_ui = Ui{};
    s_ui.f = fontpack_lv_font_ui();
    s_ui.fb = fontpack_lv_font_get(30, 4);
    if (s_ui.fb == nullptr) s_ui.fb = s_ui.f;
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_ui.top = status.height;
    s_ui.body = Box(scr);
    lv_obj_set_size(s_ui.body, LV_HOR_RES, LV_VER_RES - s_ui.top);
    lv_obj_set_pos(s_ui.body, 0, s_ui.top);
    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);
    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = MonitorScreen::Create;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    ESP_LOGI(TAG, "create");
    return scr;
}

bool MonitorScreen::IsActive() { return s_active.load(); }

void MonitorScreen::DebugCmd(char c) {
    if (c == 'r') RunTask(JobRefresh, 0);
    else if (c == '1' || c == '7') {
        s_range_days = c - '0';
        Render();
    }
}
