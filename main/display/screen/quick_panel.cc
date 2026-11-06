#include <esp_timer.h>
#include "net_mode.h"
#include "quick_panel.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "screen_common.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"

#include <atomic>
#include <cstdio>
#include <string>

#include <esp_log.h>
#include <wifi_station.h>

#include "standby_screen/standby_card.h"

namespace {

constexpr const char* TAG = "QuickPanel";
constexpr lv_coord_t kPad = 16;
constexpr lv_coord_t kRowH = 64;
constexpr uint32_t kAutoCloseMs = 30000;
constexpr int kFullEvery[] = {5, 15, 30, 60, 0};
constexpr int kFullEveryCount = sizeof(kFullEvery) / sizeof(kFullEvery[0]);
constexpr int kVolStep = 10;

std::atomic<bool> s_open{false};
std::atomic<bool> s_open_pending{false};

struct Ui {
    lv_obj_t* root = nullptr;     // 背板（整屏，点空白关闭）
    lv_obj_t* panel = nullptr;
    lv_obj_t* battery = nullptr;
    lv_obj_t* wifi = nullptr;
    lv_obj_t* vol = nullptr;
    lv_obj_t* haptic_sw = nullptr;
    lv_obj_t* mute_sw = nullptr;
    lv_obj_t* stb_card = nullptr;
    lv_obj_t* stb_clock = nullptr;
    lv_obj_t* offline_sw = nullptr;
    bool offline_want = false;
    lv_obj_t* full_btns[kFullEveryCount] = {};
    lv_timer_t* auto_close = nullptr;
    int volume = 0;  // 本地显示值（实际写入在主循环）
};
Ui s_ui;

const lv_font_t* Font() { return fontpack_lv_font_ui(); }
const lv_font_t* FontBold() {
    const lv_font_t* f = fontpack_lv_font_get(30, 4);
    return f ? f : Font();
}

lv_obj_t* Box(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* f = nullptr,
                lv_color_t c = lv_color_black()) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f ? f : Font(), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                 intptr_t ud) {
    lv_obj_t* b = Box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2 > 14 ? 14 : h / 2, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(b);
    lv_obj_t* l = Label(b, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(ud));
    return b;
}

void StyleSelected(lv_obj_t* b, bool sel) {
    lv_obj_set_style_bg_color(b, sel ? lv_color_black() : lv_color_white(), 0);
    lv_obj_t* l = lv_obj_get_child(b, 0);
    if (l) lv_obj_set_style_text_color(l, sel ? lv_color_white() : lv_color_black(), 0);
}

lv_obj_t* Switch(lv_obj_t* parent, bool on) {
    lv_obj_t* track = Box(parent);
    lv_obj_set_size(track, 64, 34);
    lv_obj_set_style_radius(track, 17, 0);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    lv_obj_set_style_bg_color(track, on ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_t* knob = Box(track);
    lv_obj_set_size(knob, 24, 24);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, on ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
    return track;
}

lv_obj_t* Separator(lv_obj_t* parent) {
    lv_obj_t* l = Box(parent);
    lv_obj_set_size(l, lv_pct(100), 1);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}

lv_obj_t* Row(lv_obj_t* parent, const char* title) {
    lv_obj_t* r = Box(parent);
    lv_obj_set_size(r, lv_pct(100), kRowH);
    lv_obj_t* t = Label(r, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    return r;
}

// ---------------------------------------------------------------- 状态刷新

void RefreshStatus() {
    if (s_ui.panel == nullptr) return;
    int level = 0;
    bool charging = false, discharging = false;
    char buf[64];
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        std::snprintf(buf, sizeof(buf), "电量 %d%%%s", level, charging ? " · 充电中" : "");
    } else {
        std::snprintf(buf, sizeof(buf), "电量 --");
    }
    lv_label_set_text(s_ui.battery, buf);

    auto& ws = WifiStation::GetInstance();
    std::string w;
    if (net_mode::Offline() || s_ui.offline_want) {
        w = "离线模式（WiFi 已关）";
    } else if (ws.IsConnected()) {
        const int rssi = ws.GetRssi();
        const char* q = rssi >= -55 ? "强" : (rssi >= -70 ? "中" : "弱");
        w = ws.GetSsid() + " · 信号" + q;
    } else if (ws.IsLinkInProgress()) {
        w = "连接中…";
    } else {
        w = "未连接";
    }
    lv_label_set_text(s_ui.wifi, w.c_str());

    std::snprintf(buf, sizeof(buf), "%d", s_ui.volume);
    lv_label_set_text(s_ui.vol, buf);

    const int cur = LVAdapterDisplay::GetFullRefreshEvery();
    for (int i = 0; i < kFullEveryCount; ++i) {
        if (s_ui.full_btns[i]) StyleSelected(s_ui.full_btns[i], kFullEvery[i] == cur);
    }
}

void RebuildHapticSwitch() {
    if (s_ui.haptic_sw == nullptr) return;
    lv_obj_t* row = lv_obj_get_parent(s_ui.haptic_sw);
    lv_obj_delete(s_ui.haptic_sw);
    s_ui.haptic_sw = Switch(row, HapticIsEnabled());
    lv_obj_align(s_ui.haptic_sw, LV_ALIGN_RIGHT_MID, 0, 0);
}

void RefreshStandbyBtns() {
    if (s_ui.stb_card) StyleSelected(s_ui.stb_card, StandbyCard_Enabled());
    if (s_ui.stb_clock) StyleSelected(s_ui.stb_clock, StandbyCard_Enabled() && StandbyCard_ClockEnabled());
}

void KickAutoClose() {
    if (s_ui.auto_close) lv_timer_reset(s_ui.auto_close);
}

// ---------------------------------------------------------------- 事件

void OnBackdrop(lv_event_t* e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) QuickPanel::Close();
}

void OnFullNow(lv_event_t*) {
    QuickPanel::Close();  // 先收起面板，再全刷当前页
    if (auto* d = LVAdapterDisplay::Instance()) d->RequestFullRefreshNow();
}

void OnFullEvery(lv_event_t* e) {
    LVAdapterDisplay::SetFullRefreshEvery(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    RefreshStatus();
    KickAutoClose();
}

void OnVolume(lv_event_t* e) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    int v = s_ui.volume + delta;
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    if (v == s_ui.volume) return;
    s_ui.volume = v;
    if (delta > 0 && Board::GetInstance().GetAudioCodec()->muted()) {  // 调大音量顺带解除静音
        Board::GetInstance().GetAudioCodec()->SetMuted(false);
        if (s_ui.mute_sw) {
            lv_obj_t* row = lv_obj_get_parent(s_ui.mute_sw);
            lv_obj_delete(s_ui.mute_sw);
            s_ui.mute_sw = Switch(row, false);
            lv_obj_align(s_ui.mute_sw, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        Application::GetInstance().Schedule([]() {
            if (auto* d = Board::GetInstance().GetDisplay()) d->UpdateStatusBar(true);
        });
    }
    RefreshStatus();
    KickAutoClose();
    // SetOutputVolume 会写 NVS：放到主循环（内部 RAM 栈）执行
    Application::GetInstance().Schedule([v]() {
        if (auto* codec = Board::GetInstance().GetAudioCodec()) codec->SetOutputVolume(v);
    });
}

void OnHaptic(lv_event_t*) {
    HapticSetEnabled(!HapticIsEnabled());
    if (HapticIsEnabled()) HapticPulseIfEnabled();
    RebuildHapticSwitch();
    KickAutoClose();
}

void OnMute(lv_event_t*) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    const bool on = !codec->muted();
    codec->SetMuted(on);
    if (s_ui.mute_sw) {
        lv_obj_t* row = lv_obj_get_parent(s_ui.mute_sw);
        lv_obj_delete(s_ui.mute_sw);
        s_ui.mute_sw = Switch(row, on);
        lv_obj_align(s_ui.mute_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    // 状态栏静音图标：放主循环刷新（这里在 LVGL 线程，别在里面再拿显示锁）
    Application::GetInstance().Schedule([]() {
        if (auto* d = Board::GetInstance().GetDisplay()) d->UpdateStatusBar(true);
    });
    KickAutoClose();
}

void OnOffline(lv_event_t*) {
    s_ui.offline_want = !s_ui.offline_want;
    const bool on = s_ui.offline_want;
    // 写 NVS：放主循环（内部 RAM 栈）
    Application::GetInstance().Schedule([on]() { net_mode::SetOffline(on); });
    if (s_ui.offline_sw) {
        lv_obj_t* row = lv_obj_get_parent(s_ui.offline_sw);
        lv_obj_delete(s_ui.offline_sw);
        s_ui.offline_sw = Switch(row, on);
        lv_obj_align(s_ui.offline_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    RefreshStatus();
    KickAutoClose();
}

void RefreshStandbyBtns();

void OnStandbyCard(lv_event_t* e) {
    const int which = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (which == 0) StandbyCard_SetEnabled(!StandbyCard_Enabled());
    else StandbyCard_SetClockEnabled(!StandbyCard_ClockEnabled());
    RefreshStandbyBtns();
    KickAutoClose();
}

void OpenNetworkSettingsAsync(void*) { ScreenNavigateTo(SettingsScreen::Create); }

void OnWifi(lv_event_t*) {
    QuickPanel::Close();
    lv_async_call(OpenNetworkSettingsAsync, nullptr);
}

void OnAutoClose(lv_timer_t*) { QuickPanel::Close(); }

void OnUpgrade(lv_event_t*) {
    std::string cur, next, url;
    if (!Application::GetInstance().GetPendingUpdate(cur, next, url)) return;
    QuickPanel::Close();
    OtaConfirmDialog::ShowAsync(cur.c_str(), next.c_str(), OtaConfirmDialog::Mode::About,
                                [url, cur, next](OtaConfirmDialog::Choice choice) {
        if (choice != OtaConfirmDialog::Choice::Upgrade) return;
        Application::GetInstance().Schedule([url, cur, next]() {
            Application::GetInstance().UpgradeFirmwareUrl(url, cur, next);
        });
    });
}

// ---------------------------------------------------------------- 打开

void OpenAsync(void*) {
    const int64_t t_build = esp_timer_get_time();
    s_open_pending.store(false);
    if (s_ui.root != nullptr) return;
    if (StandbyScreen::IsActive() || OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) return;

    if (auto* codec = Board::GetInstance().GetAudioCodec()) s_ui.volume = codec->output_volume();

    lv_obj_t* root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, OnBackdrop, LV_EVENT_CLICKED, nullptr);
    s_ui.root = root;

    // 面板从顶端垂下：下沿粗线 + 把手，像手机下拉
    lv_obj_t* p = Box(root);
    lv_obj_set_size(p, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(p, 4, 0);
    lv_obj_set_style_border_color(p, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(p, kPad, 0);
    lv_obj_set_style_pad_top(p, 14, 0);
    lv_obj_set_style_pad_bottom(p, 12, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);  // 吃掉点击，避免穿透关闭
    s_ui.panel = p;

    // 标题 + 电量
    lv_obj_t* head = Box(p);
    lv_obj_set_size(head, lv_pct(100), 48);
    lv_obj_t* title = Label(head, "快捷设置", FontBold());
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.battery = Label(head, "");
    lv_obj_align(s_ui.battery, LV_ALIGN_RIGHT_MID, 0, 0);
    Separator(p);

    // 屏幕全刷
    {
        lv_obj_t* r = Row(p, "屏幕残影");
        lv_obj_t* b = Button(r, "立即全刷", 150, 48, OnFullNow, 0);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    {
        lv_obj_t* t = Label(p, "自动全刷频率（每 N 次换页）");
        (void)t;
        lv_obj_t* grid = Box(p);
        lv_obj_set_size(grid, lv_pct(100), 56);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int i = 0; i < kFullEveryCount; ++i) {
            char buf[16];
            if (kFullEvery[i] == 0) std::snprintf(buf, sizeof(buf), "手动");
            else std::snprintf(buf, sizeof(buf), "%d", kFullEvery[i]);
            s_ui.full_btns[i] = Button(grid, buf, 76, 48, OnFullEvery, kFullEvery[i]);
        }
    }
    Separator(p);

    // 音量
    {
        lv_obj_t* r = Row(p, "音量");
        lv_obj_t* plus = Button(r, "+", 64, 48, OnVolume, kVolStep);
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
        s_ui.vol = Label(r, "");
        lv_obj_set_width(s_ui.vol, 60);
        lv_obj_set_style_text_align(s_ui.vol, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(s_ui.vol, plus, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_t* minus = Button(r, "-", 64, 48, OnVolume, -kVolStep);
        lv_obj_align_to(minus, s_ui.vol, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
    Separator(p);

    // Wi-Fi：只显示状态，点进网络设置（开关交给功耗策略，避免和自动断网冲突）
    {
        lv_obj_t* r = Row(p, "Wi-Fi");
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(r);
        lv_obj_add_event_cb(r, OnWifi, LV_EVENT_CLICKED, nullptr);
        s_ui.wifi = Label(r, "");
        lv_obj_set_width(s_ui.wifi, LV_HOR_RES - kPad * 2 - 110);
        lv_label_set_long_mode(s_ui.wifi, LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_ui.wifi, Font()->line_height);
        lv_obj_set_style_text_align(s_ui.wifi, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_ui.wifi, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    Separator(p);

    // 固件更新：后台检查到新版本才出现这一行，点了再确认升级（不主动弹窗打扰阅读）
    {
        std::string cur, next, url;
        if (Application::GetInstance().GetPendingUpdate(cur, next, url)) {
            lv_obj_t* r = Row(p, ("新固件 " + next).c_str());
            lv_obj_t* b = Button(r, "升级", 110, 48, OnUpgrade, 0);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
            Separator(p);
        }
    }

    // 待机信息卡 / 时钟
    {
        lv_obj_t* r = Row(p, "待机");
        s_ui.stb_clock = Button(r, "时钟", 100, 48, OnStandbyCard, 1);
        lv_obj_align(s_ui.stb_clock, LV_ALIGN_RIGHT_MID, 0, 0);
        s_ui.stb_card = Button(r, "信息卡", 120, 48, OnStandbyCard, 0);
        lv_obj_align_to(s_ui.stb_card, s_ui.stb_clock, LV_ALIGN_OUT_LEFT_MID, -12, 0);
        RefreshStandbyBtns();
    }
    Separator(p);

    // 离线模式：WiFi 全关不搜网，微信读书直接读缓存
    {
        s_ui.offline_want = net_mode::Offline();
        lv_obj_t* r = Row(p, "离线模式");
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, OnOffline, LV_EVENT_CLICKED, nullptr);
        s_ui.offline_sw = Switch(r, s_ui.offline_want);
        lv_obj_align(s_ui.offline_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    Separator(p);

    // 一键静音（开机默认静音；音量+ 也能解除）
    {
        lv_obj_t* r = Row(p, "静音");
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, OnMute, LV_EVENT_CLICKED, nullptr);
        s_ui.mute_sw = Switch(r, Board::GetInstance().GetAudioCodec()->muted());
        lv_obj_align(s_ui.mute_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    Separator(p);

    // 震动
    {
        lv_obj_t* r = Row(p, "触摸震动");
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, OnHaptic, LV_EVENT_CLICKED, nullptr);
        s_ui.haptic_sw = Switch(r, HapticIsEnabled());
        lv_obj_align(s_ui.haptic_sw, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    // 底部：提示 + 把手
    lv_obj_t* hint = Label(p, "点下方空白处收起");
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* grip_row = Box(p);
    lv_obj_set_size(grip_row, lv_pct(100), 14);
    lv_obj_t* grip = Box(grip_row);
    lv_obj_set_size(grip, 80, 6);
    lv_obj_set_style_radius(grip, 3, 0);
    lv_obj_set_style_bg_color(grip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_center(grip);

    s_ui.auto_close = lv_timer_create(OnAutoClose, kAutoCloseMs, nullptr);
    s_open.store(true);
    RefreshStatus();
    ESP_LOGI(TAG, "open (build %d ms)", static_cast<int>((esp_timer_get_time() - t_build) / 1000));
}

}  // namespace

namespace QuickPanel {

void RequestOpen() {
    if (s_open.load() || s_open_pending.exchange(true)) return;
    if (!ScreenLvAsync(OpenAsync)) s_open_pending.store(false);
}

bool IsOpen() { return s_open.load(); }

void Close() {
    if (s_ui.auto_close) lv_timer_delete(s_ui.auto_close);
    if (s_ui.root) lv_obj_delete(s_ui.root);
    const bool was = s_ui.root != nullptr;
    s_ui = Ui{};
    s_open.store(false);
    if (was) ESP_LOGI(TAG, "close");
}

void RequestClose() {
    if (!s_open.load()) return;
    ScreenLvAsync([](void*) { QuickPanel::Close(); });
}

}  // namespace QuickPanel
