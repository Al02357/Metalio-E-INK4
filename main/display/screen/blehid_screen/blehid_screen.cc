#include "blehid_screen.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ble_hid/ble_hid.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "SimpleUart.hpp"
#include "power_hw.h"
#include "settings.h"
#include "application.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "lv_adapter_display.h"
#include "net_mode.h"
#include "power_policy.h"
#include "sc7a20h.h"
#include "screen_common.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "BleHidUi";
constexpr const char* kScreenName = "blehid";
constexpr int kW = 480;

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* state = nullptr;
    lv_obj_t* pages[4] = {};
    lv_obj_t* tabs[4] = {};
    lv_obj_t* vbtn = nullptr;   // 语音页：按住说话
    lv_obj_t* vbtn2 = nullptr;  // 触控板页顶上的按住说话
    lv_obj_t* dot = nullptr;    // 连接状态圆点
    lv_obj_t* eng[3] = {};      // 识别方式分段按钮
    lv_obj_t* eng_chip = nullptr;  // 触控板工具栏里的识别方式
    lv_obj_t* menu = nullptr;      // 头部「配对…」弹出的小菜单
    lv_obj_t* vhint = nullptr;
    lv_obj_t* drag = nullptr;
    lv_obj_t* tilt = nullptr;
    lv_obj_t* kb = nullptr;
    const lv_font_t* f = nullptr;
    const lv_font_t* fb = nullptr;
    int top = 0;
};
Ui s_ui;
std::atomic<bool> s_active{false};
std::atomic<bool> s_want{false};
std::atomic<bool> s_start_failed{false};
std::atomic<bool> s_forget{false};  // 「忘记配对」：要写 NVS，交给内部 RAM 栈的任务做
TaskHandle_t s_ctl = nullptr;
int s_page = 0;
// 横屏键盘的键位（逻辑坐标），RawTouch 在触摸任务里直接命中、立刻发键，不等 LVGL 重画和墨水屏刷新
struct KeyHit {
    lv_area_t a;
    const char* t;
};
KeyHit s_keys[72];
std::atomic<int> s_nkeys{0};
std::atomic<uint8_t> s_mods_a{0};  // 1 Ctrl 2 Shift 4 Alt 8 Win（一次性：按完下一个键自动放开）
bool SendNamedKey(const char* t);
void ClearModsUi(void*);
lv_obj_t* s_kb_root = nullptr;   // 横屏键盘整页（不在时为空）
lv_obj_t* s_kb_state = nullptr;
bool s_inhibit = false;
void EnterKb();
void LeaveKb();
void ApplyKbState();
void ApplyState(void*);
void ApplyVoiceHint();

// ---------------------------------------------------------------- 外置蓝牙音频芯片（麦克风走它）

/**
 * 切到模式 1（本机声卡）并确认芯片有回应。芯片偶尔会卡死：不回 AT，I2S 上反复送同一段 512 点的旧数据，
 * 录出来是循环的嗡嗡声，识别只出「我」（用户实测）。主控重启救不回来，只能给它断电：拉一下 MAIN_PWR，
 * 上电后 power_hw 会自动重发模式 1。
 */
void BtChipEnsureMode1() {
    auto& uart = SimpleUart::getInstance();
    if (!uart.isInitialized()) return;
    static std::atomic<bool> s_ok{false};
    s_ok.store(false);
    uart.registerCallback([](const std::vector<uint8_t>& d) {
        std::string t(d.begin(), d.end());
        if (t.find("SET MODE 1") != std::string::npos) s_ok.store(true);
    });
    uart.sendString("AT+RX=2\r\n");
    vTaskDelay(pdMS_TO_TICKS(700));
    uart.sendString("AT+MODE=1\r\n");
    for (int i = 0; i < 15 && !s_ok.load(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_ok.load()) {
        ESP_LOGI(TAG, "bt audio chip: mode 1 ok");
        return;
    }
    ESP_LOGW(TAG, "bt audio chip not answering: power-cycle MAIN_PWR");
    power_hw_main_rail_set(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    power_hw_main_rail_set(true);  // 上升沿会异步重发模式 1
    for (int i = 0; i < 40 && !s_ok.load(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "bt audio chip after power-cycle: %s", s_ok.load() ? "mode 1 ok" : "still silent");
}

// ---------------------------------------------------------------- 蓝牙开关（独立任务，内部 RAM 栈：会读写 NVS）

void CtlTask(void*) {
    // 只在需要开/关时临时起这个任务，做完就退出，栈（内部 RAM）还回去
    while (true) {
        const bool want = s_want.load();
        const auto st = ble_hid::GetState();
        if (s_forget.exchange(false)) {
            ble_hid::ForgetPeers();
            if (s_active.load()) ScreenLvAsync(ApplyState);
            continue;
        }
        if (want && st == ble_hid::State::kOff && !s_start_failed.load()) {
            // 内部 RAM 不够 WiFi 和蓝牙同时开：先把 WiFi 整个关掉
            net_mode::SetRadioYield(true);
            if (!ble_hid::Start()) s_start_failed.store(true);
            // 麦克风走外置蓝牙音频芯片，要在模式 1（本机声卡）。设置里切到别的模式、或芯片卡死时，这里救回来
            BtChipEnsureMode1();
        } else if (!want && st != ble_hid::State::kOff) {
            ble_hid::Stop();
            net_mode::SetRadioYield(false);
        } else if (!want && net_mode::RadioYielded()) {
            net_mode::SetRadioYield(false);  // 启动失败时也要把 WiFi 还回去
        } else if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) == 0) {
            s_ctl = nullptr;
            // 退出前再对一次，防止刚好在这时有人改了 s_want 却没叫醒我们
            const bool w = s_want.load();
            const bool off = ble_hid::GetState() == ble_hid::State::kOff;
            if (s_forget.load() || (w && off && !s_start_failed.load()) || (!w && (!off || net_mode::RadioYielded()))) {
                s_ctl = xTaskGetCurrentTaskHandle();
                continue;
            }
            break;
        }
    }
    vTaskDelete(nullptr);
}

void Kick() {
    if (!s_ctl) xTaskCreatePinnedToCore(CtlTask, "ble_ctl", 8192, nullptr, 4, &s_ctl, 0);
    else xTaskNotifyGive(s_ctl);
}

// ---------------------------------------------------------------- 界面小件

lv_obj_t* Box(lv_obj_t* p) {
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* Label(lv_obj_t* p, const char* t, const lv_font_t* f = nullptr) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f ? f : s_ui.f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, t);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void SetInv(lv_obj_t* b, bool on) {
    lv_obj_set_style_bg_color(b, on ? lv_color_black() : lv_color_white(), 0);
    if (lv_obj_t* l = lv_obj_get_child(b, 0)) lv_obj_set_style_text_color(l, on ? lv_color_white() : lv_color_black(), 0);
}

lv_obj_t* Btn(lv_obj_t* p, const char* t, int x, int y, int w, int h, lv_event_cb_t cb, intptr_t ud,
              lv_event_code_t code = LV_EVENT_CLICKED) {
    lv_obj_t* b = Box(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(b);
    lv_obj_t* l = Label(b, t);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, code, reinterpret_cast<void*>(ud));
    return b;
}

/** 分段选择条：一个圆角外框，n 段等分，中间细竖线；返回各段对象 */
void Segmented(lv_obj_t* p, int x, int y, int w, int h, const char* const* labels, int n, lv_event_cb_t cb,
               lv_obj_t** out, const lv_font_t* f = nullptr) {
    lv_obj_t* box = Box(p);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    const int inner = w - 4, seg = inner / n;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* it = Box(box);
        lv_obj_set_pos(it, i * seg, 0);
        lv_obj_set_size(it, i == n - 1 ? inner - i * seg : seg, h - 4);
        lv_obj_set_style_bg_color(it, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
        if (i > 0) {
            lv_obj_set_style_border_side(it, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(it, 2, 0);
            lv_obj_set_style_border_color(it, lv_color_black(), 0);
        }
        lv_obj_add_flag(it, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_4));
        HapticAttachClick(it);
        lv_obj_t* l = Label(it, labels[i], f);
        lv_obj_center(l);
        lv_obj_add_event_cb(it, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        out[i] = it;
    }
}

// ---------------------------------------------------------------- 状态

void ApplyState(void*) {
    if (!s_active.load() || !s_ui.state) return;
    const auto st = ble_hid::GetState();
    const int pk = ble_hid::PendingPasskey();
    char buf[200];
    const lv_font_t* f = s_ui.f;
    bool filled = false;
    if (s_start_failed.load()) {
        std::snprintf(buf, sizeof(buf), "蓝牙打开失败，退出再进试试");
    } else if (pk >= 0) {
        std::snprintf(buf, sizeof(buf), "配对码 %06d", pk);
        f = s_ui.fb;
        filled = true;
    } else if (st == ble_hid::State::kConnected) {
        std::snprintf(buf, sizeof(buf), "已连接");
        filled = true;
    } else if (ble_hid::AuthFailed()) {
        std::snprintf(buf, sizeof(buf), "配对对不上：在对方蓝牙里删掉本机再连");
    } else if (st == ble_hid::State::kAdvertising) {
        std::snprintf(buf, sizeof(buf), "等待连接「Metalio E-Ink」");
    } else {
        std::snprintf(buf, sizeof(buf), "正在打开蓝牙…");
    }
    if (s_ui.dot) lv_obj_set_style_bg_color(s_ui.dot, filled ? lv_color_black() : lv_color_white(), 0);
    ApplyKbState();
    ApplyVoiceHint();
    if (std::strcmp(lv_label_get_text(s_ui.state), buf) == 0) return;
    lv_obj_set_style_text_font(s_ui.state, f, 0);
    lv_label_set_text(s_ui.state, buf);
}

void OnBleState() {
    if (s_active.load()) ScreenLvAsync(ApplyState);
}

void ShowPage(int i) {
    if (i == 1) {  // 键盘：横屏整页盖上去，底下保持原来那页
        EnterKb();
        return;
    }
    LeaveKb();
    s_page = i;
    for (int k = 0; k < 4; ++k) {
        if (!s_ui.pages[k]) continue;
        if (k == i) lv_obj_clear_flag(s_ui.pages[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.pages[k], LV_OBJ_FLAG_HIDDEN);
        SetInv(s_ui.tabs[k], k == i);
    }
}

void OnTab(lv_event_t* e) { ShowPage(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }

void OnMenuClose(lv_event_t*) {
    if (!s_ui.menu) return;
    lv_obj_t* m = s_ui.menu;
    s_ui.menu = nullptr;
    lv_obj_delete_async(m);
}

void OnForget(lv_event_t*);

void OnMenuOpen(lv_event_t*) {
    if (s_ui.menu || !s_ui.scr) return;
    // 遮罩 + 小卡片：说明 + 「忘记所有配对」+「取消」
    lv_obj_t* mask = Box(s_ui.scr);
    s_ui.menu = mask;
    lv_obj_set_size(mask, kW, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_white(), 0);  // 墨水屏没有半透明：整屏盖白，只留卡片
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, OnMenuClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* card = Box(mask);
    lv_obj_set_size(card, kW - 40, 250);
    lv_obj_set_pos(card, 20, (LV_VER_RES - 250) / 2);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // 点卡片本身不关
    lv_obj_t* t = Label(card, "忘记配对后，所有电脑和手机都要在它们的蓝牙里删掉「Metalio E-Ink」再重新添加。");
    lv_obj_set_width(t, kW - 80);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(t, 18, 16);
    const int bw = (kW - 40 - 18 * 2 - 12) / 2;
    Btn(card, "取消", 18, 250 - 18 - 64, bw, 64, OnMenuClose, 0);
    lv_obj_t* f = Btn(card, "忘记配对", 18 + bw + 12, 250 - 18 - 64, bw, 64, OnForget, 0);
    SetInv(f, true);
    lv_obj_move_foreground(mask);
}

void OnForget(lv_event_t* e) {
    OnMenuClose(e);
    // 不能在 LVGL 任务里直接清：它的栈在 PSRAM，写 NVS 时关 Cache 会崩（之前一点就重启）
    s_forget.store(true);
    Kick();
}

// ---------------------------------------------------------------- 触控板

struct Pad {
    lv_point_t last{};
    int64_t t0 = 0;
    int travel = 0;
    bool moved = false, long_done = false;
    float rx = 0, ry = 0;
    int wheel_acc = 0;
};
Pad s_pad;
bool s_drag = false;

uint8_t HeldButtons() { return s_drag ? 1 : 0; }

void Click(uint8_t b) {
    ble_hid::MouseButtons(HeldButtons() | b);
    ble_hid::MouseButtons(HeldButtons());
}

// 触控板 / 滚动条的屏幕区域（逻辑坐标），BuildPad 时记下
lv_area_t s_pad_area{}, s_strip_area{};
enum class RawMode : uint8_t { kNone, kPad, kStrip, kKey, kTilt };
RawMode s_raw_mode = RawMode::kNone;
std::atomic<bool> s_pad_moved{false};

bool InArea(const lv_area_t& a, int x, int y) { return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2; }

// touch_feed 任务里调用（约 8ms 一次）：不经过 LVGL 的 30ms 输入节拍，鼠标回报率能到 100Hz 以上
int64_t s_tilt_down_us = 0;
lv_area_t s_tilt_area{};
void TiltTapMain(void* p);
void RawTouch(bool down, int x, int y) {
    if (!down) {
        if (s_raw_mode == RawMode::kTilt && s_tilt_down_us) {
            const int64_t held = esp_timer_get_time() - s_tilt_down_us;
            s_tilt_down_us = 0;
            ScreenLvAsync(TiltTapMain, held >= 600000 ? reinterpret_cast<void*>(1) : nullptr);
        }
        s_raw_mode = RawMode::kNone;
        return;
    }
    if (s_raw_mode == RawMode::kNone) {
        if (s_kb_root) {
            const int n = s_nkeys.load();
            for (int i = 0; i < n; ++i) {
                if (InArea(s_keys[i].a, x, y)) {
                    SendNamedKey(s_keys[i].t);  // 修饰键 / 切换键不是普通键，留给 LVGL 的 OnKey
                    break;
                }
            }
            s_raw_mode = RawMode::kKey;  // 这次按住期间不再命中
            return;
        }
        if (s_page != 0) return;
        if (InArea(s_tilt_area, x, y)) {
            s_raw_mode = RawMode::kTilt;
            s_tilt_down_us = esp_timer_get_time();
            return;
        }
        if (InArea(s_pad_area, x, y)) s_raw_mode = RawMode::kPad;
        else if (InArea(s_strip_area, x, y)) s_raw_mode = RawMode::kStrip;
        else return;
        s_pad.last = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
        s_pad.travel = 0;
        s_pad.rx = s_pad.ry = 0;
        s_pad.wheel_acc = 0;
        s_pad_moved.store(false);
        return;
    }
    const int dx = x - s_pad.last.x, dy = y - s_pad.last.y;
    s_pad.last = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
    if (s_raw_mode == RawMode::kKey || s_raw_mode == RawMode::kTilt) return;
    if (s_raw_mode == RawMode::kStrip) {
        s_pad.wheel_acc += dy;
        int steps = 0;
        while (s_pad.wheel_acc >= 18) {
            s_pad.wheel_acc -= 18;
            --steps;  // 手指往下 = 往下滚
        }
        while (s_pad.wheel_acc <= -18) {
            s_pad.wheel_acc += 18;
            ++steps;
        }
        if (steps) ble_hid::Mouse(0, 0, steps, HeldButtons());
        return;
    }
    s_pad.travel += std::abs(dx) + std::abs(dy);
    if (!s_pad_moved.load() && s_pad.travel < 8) return;  // 先过个门槛，免得点击时手抖
    s_pad_moved.store(true);
    const float sp = std::sqrt(static_cast<float>(dx * dx + dy * dy));
    const float g = 1.2f + std::fmin(sp / 6.0f, 2.5f);  // 采样快了，单次位移小，加速门槛跟着降
    s_pad.rx += dx * g;
    s_pad.ry += dy * g;
    const int ox = static_cast<int>(s_pad.rx), oy = static_cast<int>(s_pad.ry);
    s_pad.rx -= ox;
    s_pad.ry -= oy;
    if (ox || oy) ble_hid::Mouse(ox, oy, 0, HeldButtons());
}

void OnPad(lv_event_t* e) {
    // 移动在 RawTouch 里发；这里只判断轻点 / 长按
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_pad.t0 = esp_timer_get_time();
        s_pad.long_done = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (!s_pad_moved.load()) {
            s_pad.long_done = true;
            Click(2);  // 长按 = 右键
        }
    } else if (code == LV_EVENT_RELEASED) {
        const int64_t dur = esp_timer_get_time() - s_pad.t0;
        if (!s_pad_moved.load() && !s_pad.long_done && dur < 350000) Click(1);  // 轻点 = 左键
    }
}

void OnMouseBtn(lv_event_t* e) {
    const int b = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    Click(static_cast<uint8_t>(b));
}

void OnDrag(lv_event_t*) {
    s_drag = !s_drag;
    SetInv(s_ui.drag, s_drag);
    ble_hid::MouseButtons(HeldButtons());
}

// ---- 倾斜光标：加速度计相对开启那一刻的姿态

std::atomic<bool> s_tilt{false};
TaskHandle_t s_tilt_task = nullptr;
bool s_axis_swap = true;  // 用户实测：x/y 对调且都反向
int s_axis_sx = -1, s_axis_sy = 1;  // 上下方向用户实测后又反了一次

// 倾斜死区（mg，约 1mg ≈ 0.057°）：长按「倾斜」在 小 / 中 / 大 之间换
const int kTiltDead[3] = {90, 140, 210};  // 约 5° / 8° / 12°
const char* const kTiltDeadName[3] = {"小", "中", "大"};
std::atomic<int> s_tilt_dead{1};

void TiltTask(void*) {
    auto& acc = Sc7a20h::GetInstance();
    // 起点取 8 次平均：开倾斜那一刻的姿势当「不动」
    float bx = 0, by = 0;
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        int ax = 0, ay = 0, az = 0;
        if (acc.ReadAccelMg(ax, ay, az)) {
            bx += ax;
            by += ay;
            ++n;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (n) {
        bx /= n;
        by /= n;
    }
    float fx = 0, fy = 0;  // 低通后的偏移，滤掉手抖
    float rx = 0, ry = 0;
    while (s_tilt.load()) {
        int ax = 0, ay = 0, az = 0;
        if (acc.ReadAccelMg(ax, ay, az)) {
            float dx = ax - bx, dy = ay - by;
            if (s_axis_swap) std::swap(dx, dy);
            dx *= s_axis_sx;
            dy *= s_axis_sy;
            fx += (dx - fx) * 0.35f;
            fy += (dy - fy) * 0.35f;
            // 圆形死区：倾斜幅度在死区内不动；出了死区后速度从 0 平滑往上走（平方曲线，小角度更好控）
            const float dead = static_cast<float>(kTiltDead[s_tilt_dead.load()]);
            const float mag = std::sqrt(fx * fx + fy * fy);
            if (mag > dead) {
                const float over = mag - dead;
                const float v = std::fmin(over * over * 0.00045f + over * 0.012f, 18.0f);  // 每 20ms 最多 18 像素
                rx += fx / mag * v;
                ry += fy / mag * v;
                const int ox = static_cast<int>(rx), oy = static_cast<int>(ry);
                rx -= ox;
                ry -= oy;
                if (ox || oy) ble_hid::Mouse(ox, oy, 0, HeldButtons());
            } else {
                rx = ry = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_tilt_task = nullptr;
    vTaskDelete(nullptr);
}

void ApplyTiltLabel() {
    if (!s_ui.tilt) return;
    char b[32];
    std::snprintf(b, sizeof(b), "倾斜·%s", kTiltDeadName[s_tilt_dead.load()]);
    lv_label_set_text(lv_obj_get_child(s_ui.tilt, 0), b);
}

void SetTilt(bool on) {
    if (on == s_tilt.load()) return;
    s_tilt.store(on);
    if (on && !s_tilt_task) xTaskCreatePinnedToCore(TiltTask, "ble_tilt", 3072, nullptr, 4, &s_tilt_task, 1);
    if (s_ui.tilt) SetInv(s_ui.tilt, on);
}

// 「倾斜」键：短按开关，按住超过 0.6 秒换死区。按住时长在原始触摸里量（LVGL 读触摸跟着墨水屏刷新走，长按不准）
void TiltTapMain(void* p) {
    if (p) {
        s_tilt_dead.store((s_tilt_dead.load() + 1) % 3);
        ApplyTiltLabel();
    } else {
        SetTilt(!s_tilt.load());
    }
}
void OnTilt(lv_event_t*) {}  // 动作在 RawTouch 里

void OnTalk(lv_event_t* e);
void OnEnter(lv_event_t*);
void OnBackspace(lv_event_t*);
void OnEngineCycle(lv_event_t*);

void BuildPad(lv_obj_t* pg, int h) {
    // 顶上一条工具栏：按住说话 | 识别方式（点一下换）| 退格 | 回车。和「语音」页同一套设置
    const int th = 68;
    {
        lv_obj_t* bar = Box(pg);
        lv_obj_set_pos(bar, 10, 0);
        lv_obj_set_size(bar, kW - 20, th);
        lv_obj_set_style_radius(bar, 12, 0);
        lv_obj_set_style_border_width(bar, 2, 0);
        lv_obj_set_style_border_color(bar, lv_color_black(), 0);
        lv_obj_set_style_clip_corner(bar, true, 0);
        const int iw = kW - 24, wt = 186, we = 104, wk = (iw - wt - we) / 2;
        const int xs[4] = {0, wt, wt + we, wt + we + wk};
        const int ws[4] = {wt, we, wk, iw - (wt + we + wk)};
        lv_obj_t* it[4];
        for (int i = 0; i < 4; ++i) {
            it[i] = Box(bar);
            lv_obj_set_pos(it[i], xs[i], 0);
            lv_obj_set_size(it[i], ws[i], th - 4);
            lv_obj_set_style_bg_color(it[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(it[i], LV_OPA_COVER, 0);
            if (i > 0) {
                lv_obj_set_style_border_side(it[i], LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(it[i], 2, 0);
                lv_obj_set_style_border_color(it[i], lv_color_black(), 0);
            }
            lv_obj_add_flag(it[i], static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_4));
            lv_obj_clear_flag(it[i], static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN));
            HapticAttachClick(it[i]);
        }
        lv_obj_center(Label(it[0], "按住说话"));
        for (auto c : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) lv_obj_add_event_cb(it[0], OnTalk, c, nullptr);
        s_ui.vbtn2 = it[0];
        lv_obj_center(Label(it[1], "本机"));
        lv_obj_add_event_cb(it[1], OnEngineCycle, LV_EVENT_CLICKED, nullptr);
        s_ui.eng_chip = it[1];
        lv_obj_center(Label(it[2], "退格"));
        lv_obj_add_event_cb(it[2], OnBackspace, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(it[2], OnBackspace, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
        lv_obj_center(Label(it[3], "回车"));
        lv_obj_add_event_cb(it[3], OnEnter, LV_EVENT_PRESSED, nullptr);
    }
    const int top = th + 10;
    const int bh = 76, ph = h - bh - 16 - top;
    lv_obj_t* pad = Box(pg);
    lv_obj_set_pos(pad, 10, top);
    lv_obj_set_size(pad, 382, ph);
    lv_obj_set_style_radius(pad, 12, 0);
    lv_obj_set_style_border_width(pad, 2, 0);
    lv_obj_set_style_border_color(pad, lv_color_black(), 0);
    lv_obj_add_flag(pad, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_4));  // USER_4：按下不反色
    lv_obj_clear_flag(pad, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_t* hint = Label(pad, "滑动移动光标\n轻点 = 左键\n长按 = 右键");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint);
    for (auto c : {LV_EVENT_PRESSED, LV_EVENT_LONG_PRESSED, LV_EVENT_RELEASED}) lv_obj_add_event_cb(pad, OnPad, c, nullptr);

    lv_obj_t* strip = Box(pg);
    lv_obj_set_pos(strip, 400, top);
    lv_obj_set_size(strip, 70, ph);
    lv_obj_set_style_radius(strip, 12, 0);
    lv_obj_set_style_border_width(strip, 2, 0);
    lv_obj_set_style_border_color(strip, lv_color_black(), 0);
    lv_obj_add_flag(strip, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_4));
    lv_obj_clear_flag(strip, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_t* sl = Label(strip, "滚\n动");
    lv_obj_center(sl);
    // 滚动在 RawTouch 里处理

    // 记下逻辑坐标区域给 RawTouch（页面 y 偏移 = 页容器的 y）
    lv_obj_update_layout(pg);
    lv_obj_update_layout(pad);
    lv_obj_get_coords(pad, &s_pad_area);
    lv_obj_get_coords(strip, &s_strip_area);
    const int y = top + ph + 12, w = 108, gap = 8;
    Btn(pg, "左键", 10, y, w, bh, OnMouseBtn, 1);
    Btn(pg, "右键", 10 + (w + gap), y, w, bh, OnMouseBtn, 2);
    s_ui.drag = Btn(pg, "拖动", 10 + 2 * (w + gap), y, w, bh, OnDrag, 0);
    s_ui.tilt = Btn(pg, "倾斜", 10 + 3 * (w + gap), y, w, bh, OnTilt, 0, LV_EVENT_PRESSED);
    lv_obj_update_layout(s_ui.tilt);
    lv_obj_get_coords(s_ui.tilt, &s_tilt_area);
    ApplyTiltLabel();
    SetInv(s_ui.drag, s_drag);
}

// ---------------------------------------------------------------- 键盘（横屏：整页盖住，按键和文字都转 90°）

// 照普通键盘排：每行都是 15 个单位宽，左右对齐，行与行按常见的错位排（Tab 1.5、大写位 1.75、Shift 2…）
struct KeyDef {
    const char* t;
    float u;  // 宽度（单位）
};
const KeyDef kRowsLetters[5][15] = {
    {{"Esc", 1}, {"1", 1}, {"2", 1}, {"3", 1}, {"4", 1}, {"5", 1}, {"6", 1}, {"7", 1}, {"8", 1}, {"9", 1}, {"0", 1},
     {"-", 1}, {"=", 1}, {"退格", 2}},
    {{"Tab", 1.5f}, {"q", 1}, {"w", 1}, {"e", 1}, {"r", 1}, {"t", 1}, {"y", 1}, {"u", 1}, {"i", 1}, {"o", 1},
     {"p", 1}, {"[", 1}, {"]", 1}, {"\\", 1.5f}},
    {{"符号", 1.75f}, {"a", 1}, {"s", 1}, {"d", 1}, {"f", 1}, {"g", 1}, {"h", 1}, {"j", 1}, {"k", 1}, {"l", 1},
     {";", 1}, {"'", 1}, {"回车", 2.25f}},
    {{"Shift", 2}, {"z", 1}, {"x", 1}, {"c", 1}, {"v", 1}, {"b", 1}, {"n", 1}, {"m", 1}, {",", 1}, {".", 1},
     {"/", 1}, {"Del", 1}, {"↑", 1}, {"Shift", 1}},
    {{"Ctrl", 1.5f}, {"Win", 1.25f}, {"Alt", 1.25f}, {"空格", 7}, {"`", 1}, {"←", 1}, {"↓", 1}, {"→", 1}},
};
// 符号层：功能键、导航键；常用符号也可以 Shift + 字母层的键打
const KeyDef kRowsSymbols[5][15] = {
    {{"Esc", 1}, {"F1", 1}, {"F2", 1}, {"F3", 1}, {"F4", 1}, {"F5", 1}, {"F6", 1}, {"F7", 1}, {"F8", 1}, {"F9", 1},
     {"F10", 1}, {"F11", 1}, {"F12", 1}, {"退格", 2}},
    {{"Tab", 1.5f}, {"!", 1}, {"@", 1}, {"#", 1}, {"$", 1}, {"%", 1}, {"^", 1}, {"&", 1}, {"*", 1}, {"(", 1},
     {")", 1}, {"_", 1}, {"+", 1}, {"|", 1.5f}},
    {{"字母", 1.75f}, {"Home", 1}, {"End", 1}, {"PgUp", 1}, {"PgDn", 1}, {"~", 1}, {"{", 1}, {"}", 1}, {":", 1},
     {"\"", 1}, {"<", 1}, {">", 1}, {"回车", 2.25f}},
    {{"Shift", 2}, {"Ins", 1}, {"PrtSc", 1}, {"?", 1}, {"-", 1}, {"=", 1}, {"[", 1}, {"]", 1}, {",", 1}, {".", 1},
     {"/", 1}, {"Del", 1}, {"↑", 1}, {"Shift", 1}},
    {{"Ctrl", 1.5f}, {"Win", 1.25f}, {"Alt", 1.25f}, {"空格", 7}, {"`", 1}, {"←", 1}, {"↓", 1}, {"→", 1}},
};
bool s_symbols = false;

bool s_kb_ccw = true;  // true：设备逆时针转（竖屏顶边在左）；false：顺时针（顶边在右）
bool s_kb_auto = true;  // 重力自动转向（键盘打开时跟着设备往哪边横）
lv_timer_t* s_kb_timer = nullptr;
int s_kb_flip_cnt = 0;

uint8_t ModBit(const char* t) {
    if (!std::strcmp(t, "Ctrl")) return 0x01;
    if (!std::strcmp(t, "Shift")) return 0x02;
    if (!std::strcmp(t, "Alt")) return 0x04;
    if (!std::strcmp(t, "Win")) return 0x08;
    return 0;
}

uint8_t NamedUsage(const char* t) {
    struct K {
        const char* n;
        uint8_t u;
    };
    static const K kKeys[] = {{"空格", 0x2C}, {"回车", 0x28}, {"退格", 0x2A}, {"Esc", 0x29},  {"Tab", 0x2B},
                              {"←", 0x50},   {"→", 0x4F},   {"↑", 0x52},   {"↓", 0x51},   {"Del", 0x4C},
                              {"Home", 0x4A}, {"End", 0x4D}, {"PgUp", 0x4B}, {"PgDn", 0x4E}, {"Ins", 0x49},
                              {"PrtSc", 0x46}, {"F1", 0x3A},  {"F2", 0x3B},  {"F3", 0x3C},  {"F4", 0x3D},
                              {"F5", 0x3E},   {"F6", 0x3F},  {"F7", 0x40},  {"F8", 0x41},  {"F9", 0x42},
                              {"F10", 0x43},  {"F11", 0x44}, {"F12", 0x45}};
    for (const auto& k : kKeys)
        if (!std::strcmp(k.n, t)) return k.u;
    return 0;
}

/** 横屏坐标 (lx, ly, lw, lh) → 竖屏对象位置；横屏 x 沿竖屏 y */
void PlaceLand(lv_obj_t* o, int lx, int ly, int lw, int lh) {
    if (s_kb_ccw) lv_obj_set_pos(o, kW - ly - lh, lx);  // 横屏上方 = 竖屏右边
    else lv_obj_set_pos(o, ly, LV_VER_RES - lx - lw);  // 横屏上方 = 竖屏左边
    lv_obj_set_size(o, lh, lw);
}

/** 转 90° 的文字，居中放在 parent 里 */
lv_obj_t* RotLabel(lv_obj_t* parent, const char* t, int pw, int ph, const lv_font_t* f = nullptr) {
    lv_obj_t* l = Label(parent, t, f);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    // 先按横屏方向排版（宽 = 竖屏里的高），再整体转过去
    lv_obj_set_width(l, ph);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_update_layout(l);
    const int lh = lv_obj_get_height(l);
    lv_obj_set_pos(l, (pw - ph) / 2, (ph - lh) / 2);
    lv_obj_set_style_transform_pivot_x(l, ph / 2, 0);
    lv_obj_set_style_transform_pivot_y(l, lh / 2, 0);
    lv_obj_set_style_transform_rotation(l, s_kb_ccw ? 900 : -900, 0);
    return l;
}

void BuildKbRoot();

void OnKey(lv_event_t* e) {
    lv_obj_t* b = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* t = static_cast<const char*>(lv_event_get_user_data(e));
    if (!std::strcmp(t, "符号") || !std::strcmp(t, "字母")) {
        s_symbols = !s_symbols;
        ScreenLvAsync([](void*) { BuildKbRoot(); });
        return;
    }
    if (const uint8_t bit = ModBit(t)) {
        const uint8_t m = s_mods_a.fetch_xor(bit) ^ bit;
        SetInv(b, (m & bit) != 0);
        return;
    }
    // 普通键已在 RawTouch（触摸任务）里发出，这里不再发
}

/** 发一个普通键（修饰键按一次性规则用掉）；不是普通键返回 false */
bool SendNamedKey(const char* t) {
    uint8_t usage = NamedUsage(t);
    bool shift = false;
    if (!usage && t[0] && !t[1]) ble_hid::AsciiToUsage(t[0], usage, shift);
    if (!usage) return false;
    const uint8_t mods = s_mods_a.exchange(0);
    ble_hid::KeyTap(static_cast<uint8_t>(mods | (shift ? 0x02 : 0)), usage);
    if (mods) ScreenLvAsync(ClearModsUi);
    return true;
}

void ClearModsUi(void*) {
    if (!s_kb_root) return;
    const uint32_t n = lv_obj_get_child_count(s_kb_root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(s_kb_root, i);
        const char* ct = static_cast<const char*>(lv_obj_get_user_data(c));
        if (ct && ModBit(ct)) SetInv(c, false);
    }
}

void OnKbFlip(lv_event_t*) {
    s_kb_ccw = !s_kb_ccw;
    s_kb_auto = false;  // 手动转了就别再被重力转回去
    ScreenLvAsync([](void*) { BuildKbRoot(); });
}

void OnKbGravity(lv_event_t*) {
    s_kb_auto = !s_kb_auto;
    ScreenLvAsync([](void*) { BuildKbRoot(); });
}

void KbGravityTick(lv_timer_t*) {
    if (!s_kb_root || !s_kb_auto) {
        s_kb_flip_cnt = 0;
        return;
    }
    int ax = 0, ay = 0, az = 0;
    if (!Sc7a20h::GetInstance().ReadAccelMg(ax, ay, az)) return;
    const bool want_ccw = ay > 0;
    if ((ay > 600 || ay < -600) && want_ccw != s_kb_ccw) {
        if (++s_kb_flip_cnt >= 2) {  // 连着两次（约 1 秒）都是反的才转，免得晃一下就转
            s_kb_flip_cnt = 0;
            s_kb_ccw = want_ccw;
            BuildKbRoot();
        }
    } else {
        s_kb_flip_cnt = 0;
    }
}

void OnKbExit(lv_event_t*) {
    ScreenLvAsync([](void*) { ShowPage(0); });  // 不能在按键自己的事件里删掉它
}

lv_obj_t* LandBtn(const char* t, int lx, int ly, int lw, int lh, lv_event_cb_t cb, void* ud,
                  lv_event_code_t code = LV_EVENT_PRESSED) {
    lv_obj_t* b = Box(s_kb_root);
    PlaceLand(b, lx, ly, lw, lh);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    // USER_4：按下不反色。反色要重画 + 刷墨水屏，连打时动画排队半天播不完；按下反馈只靠震动
    lv_obj_add_flag(b, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE |
                                                  LV_OBJ_FLAG_USER_4));
    HapticAttachClick(b);
    // 窄键上的长标签（PgUp、PrtSc…）用小字
    const lv_font_t* small = fontpack_lv_font_get(18, 2);
    RotLabel(b, t, lh, lw, (std::strlen(t) >= 4 && lw < 70 && small) ? small : nullptr);
    lv_obj_set_user_data(b, const_cast<char*>(t));
    lv_obj_add_event_cb(b, cb, code, ud);  // 键盘按下就发（竖屏版按钮矩阵也是按下触发），不等松手
    return b;
}

void ApplyKbState();
void ApplyState(void*);
void ApplyVoiceHint();

void BuildKbRoot() {
    if (!s_ui.scr) return;
    if (s_kb_root) lv_obj_delete(s_kb_root);
    s_kb_root = Box(s_ui.scr);
    lv_obj_set_pos(s_kb_root, 0, 0);
    lv_obj_set_size(s_kb_root, kW, LV_VER_RES);
    lv_obj_set_style_bg_color(s_kb_root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_kb_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_kb_root, LV_OBJ_FLAG_CLICKABLE);  // 挡住下面的触控板
    // 横屏可用区：竖屏状态栏占掉横屏一侧
    const int lx0 = s_kb_ccw ? s_ui.top + 6 : 6, lx1 = s_kb_ccw ? LV_VER_RES - 6 : LV_VER_RES - s_ui.top - 6;
    const int lw = lx1 - lx0;
    // 顶上一条：状态 + 退出
    const int bar = 54;
    s_kb_state = Box(s_kb_root);
    PlaceLand(s_kb_state, lx0, 0, lw - 3 * 128, bar);
    lv_obj_add_flag(s_kb_state, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* gb = LandBtn(s_kb_auto ? "重力:开" : "重力:关", lx1 - 3 * 128 + 8, 4, 120, bar - 8, OnKbGravity, nullptr,
                           LV_EVENT_CLICKED);
    lv_obj_clear_flag(gb, LV_OBJ_FLAG_USER_4);
    lv_obj_t* fb = LandBtn("转 180°", lx1 - 2 * 128 + 8, 4, 120, bar - 8, OnKbFlip, nullptr, LV_EVENT_CLICKED);
    lv_obj_clear_flag(fb, LV_OBJ_FLAG_USER_4);
    lv_obj_t* xb = LandBtn("退出键盘", lx1 - 120, 4, 120, bar - 8, OnKbExit, nullptr, LV_EVENT_CLICKED);
    lv_obj_clear_flag(xb, LV_OBJ_FLAG_USER_4);
    ApplyKbState();
    const int gap = 5, rows = 5, ky0 = bar + 4;
    const int kh = (kW - ky0 - 4 - (rows - 1) * gap) / rows;
    const auto& rowsdef = s_symbols ? kRowsSymbols : kRowsLetters;
    s_nkeys.store(0);
    int nk = 0;
    const float unit = (lw + gap) / 15.0f;  // 每行 15 个单位
    for (int r = 0; r < rows; ++r) {
        float u = 0;
        for (int i = 0; i < 15 && rowsdef[r][i].t; ++i) {
            const char* t = rowsdef[r][i].t;
            const int x = lx0 + static_cast<int>(u * unit + 0.5f);
            u += rowsdef[r][i].u;
            const int w = lx0 + static_cast<int>(u * unit + 0.5f) - gap - x;
            lv_obj_t* b = LandBtn(t, x, ky0 + r * (kh + gap), w, kh, OnKey, const_cast<char*>(t));
            if (ModBit(t) && (s_mods_a.load() & ModBit(t))) SetInv(b, true);
            if (nk < 72) {
                lv_obj_update_layout(b);
                lv_obj_get_coords(b, &s_keys[nk].a);
                s_keys[nk].t = t;
                ++nk;
            }
        }
    }
    s_nkeys.store(nk);
    lv_obj_move_foreground(s_kb_root);
}

void ApplyKbState() {
    if (!s_kb_state) return;
    lv_obj_clean(s_kb_state);
    const auto st = ble_hid::GetState();
    const int pk = ble_hid::PendingPasskey();
    char buf[64];
    if (pk >= 0) std::snprintf(buf, sizeof(buf), "配对码 %06d", pk);
    else std::snprintf(buf, sizeof(buf), "%s",
                       st == ble_hid::State::kConnected ? "已连接" : (ble_hid::AuthFailed() ? "配对对不上，请重新配对" : "未连接"));
    RotLabel(s_kb_state, buf, lv_obj_get_width(s_kb_state), lv_obj_get_height(s_kb_state));
}

/** 进键盘页：按重力决定往哪边横 */
void EnterKb() {
    int ax = 0, ay = 0, az = 0;
    if (!s_kb_timer) s_kb_timer = lv_timer_create(KbGravityTick, 500, nullptr);
    if (s_kb_auto && Sc7a20h::GetInstance().ReadAccelMg(ax, ay, az)) {
        if (ay > 500) s_kb_ccw = true;
        else if (ay < -500) s_kb_ccw = false;
    }
    BuildKbRoot();
}

void LeaveKb() {
    s_nkeys.store(0);
    if (s_kb_timer) {
        lv_timer_delete(s_kb_timer);
        s_kb_timer = nullptr;
    }
    if (s_kb_root) lv_obj_delete(s_kb_root);
    s_kb_root = nullptr;
    s_kb_state = nullptr;
}

// ---------------------------------------------------------------- 遥控：上下两大块翻页，适合盲按

enum class FlipKind : uint8_t { kKey, kConsumer, kWheel, kTap };
struct PageMode {
    const char* name;
    FlipKind kind;
    uint16_t prev, next;
};
// 滚轮：鼠标滚轮滚一格（电脑网页 / 文档、手机短视频）；左右点：手机阅读 App 点屏幕两侧翻页；
// 方向键：电脑放映 / 看图 / 网页；音量键：开了「音量键翻页」的阅读 App；翻页键：PgUp/PgDn
const PageMode kPageModes[] = {{"滚轮", FlipKind::kWheel, 0, 0},   {"左右点", FlipKind::kTap, 0, 0},
                               {"方向键", FlipKind::kKey, 0x50, 0x4F}, {"音量键", FlipKind::kConsumer, 0xE9, 0xEA},
                               {"翻页键", FlipKind::kKey, 0x4B, 0x4E}};
constexpr int kPageModeCount = sizeof(kPageModes) / sizeof(kPageModes[0]);
int s_page_mode = 0;
lv_obj_t* s_mode_btn = nullptr;

void OnFlip(lv_event_t* e) {
    const bool next = lv_event_get_user_data(e) != nullptr;
    const auto& m = kPageModes[s_page_mode];
    switch (m.kind) {
        case FlipKind::kWheel:  // 鼠标滚轮滚一格：下一页 = 往下滚
            ble_hid::Mouse(0, 0, next ? -1 : 1, 0);
            break;
        case FlipKind::kTap:
            ble_hid::Tap(next ? 9000 : 1000, 5000);
            break;
        case FlipKind::kConsumer:
            ble_hid::Consumer(next ? m.next : m.prev);
            break;
        case FlipKind::kKey:
            ble_hid::KeyTap(0, static_cast<uint8_t>(next ? m.next : m.prev));
            break;
    }
}

void OnPageMode(lv_event_t*) {
    s_page_mode = (s_page_mode + 1) % kPageModeCount;
    char b[48];
    std::snprintf(b, sizeof(b), "翻页：%s（点按切换）", kPageModes[s_page_mode].name);
    lv_label_set_text(lv_obj_get_child(s_mode_btn, 0), b);
}

void OnMedia(lv_event_t* e) { ble_hid::Consumer(static_cast<uint16_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }

void BuildRemote(lv_obj_t* pg, int h) {
    const int gap = 10, bigh = (h - 54 - 64 - 3 * gap - 4) / 2;
    // 按下就发、不做按下反色（反色要等墨水屏刷新，盲按靠震动反馈）
    lv_obj_t* up = Btn(pg, "上一页", 10, 0, kW - 20, bigh, OnFlip, 0, LV_EVENT_PRESSED);
    lv_obj_t* dn = Btn(pg, "下一页", 10, bigh + gap, kW - 20, bigh, OnFlip, 1, LV_EVENT_PRESSED);
    lv_obj_add_flag(up, LV_OBJ_FLAG_USER_4);
    lv_obj_add_flag(dn, LV_OBJ_FLAG_USER_4);
    lv_obj_set_style_text_font(lv_obj_get_child(up, 0), s_ui.fb, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(dn, 0), s_ui.fb, 0);
    int y = 2 * (bigh + gap);
    char mb[48];
    std::snprintf(mb, sizeof(mb), "翻页：%s（点按切换）", kPageModes[s_page_mode].name);
    s_mode_btn = Btn(pg, mb, 10, y, kW - 20, 54, OnPageMode, 0);
    y += 54 + gap;
    const int w = (kW - 20 - 2 * gap) / 3;
    lv_obj_t* m1 = Btn(pg, "播放 / 暂停", 10, y, w, 64, OnMedia, 0xCD, LV_EVENT_PRESSED);
    lv_obj_t* m2 = Btn(pg, "音量 −", 10 + (w + gap), y, w, 64, OnMedia, 0xEA, LV_EVENT_PRESSED);
    lv_obj_t* m3 = Btn(pg, "音量 +", 10 + 2 * (w + gap), y, w, 64, OnMedia, 0xE9, LV_EVENT_PRESSED);
    for (auto* o : {m1, m2, m3}) lv_obj_add_flag(o, LV_OBJ_FLAG_USER_4);
}

// ---------------------------------------------------------------- 语音：按住说话，麦克风声音经蓝牙发给电脑端程序

std::atomic<bool> s_talking{false};
// 识别方式：0 电脑本机 1 自己服务器 2 MiMo；存 NVS（只在主任务里读写：LVGL 栈在 PSRAM，碰 NVS 会崩）
std::atomic<int> s_engine{0};
lv_obj_t* s_vresult = nullptr;
std::string s_result_text;
std::mutex s_result_mu;

const char* const kEngineShort[3] = {"本机", "服务器", "MiMo"};

void ApplyEngineUi(void*) {
    for (int i = 0; i < 3; ++i)
        if (s_ui.eng[i]) SetInv(s_ui.eng[i], i == s_engine.load());
    if (s_ui.eng_chip) lv_label_set_text(lv_obj_get_child(s_ui.eng_chip, 0), kEngineShort[s_engine.load()]);
}

void LoadEngineMain() {
    Settings st("blehid", false);
    int v = st.GetInt("asr", 0);
    s_engine.store(v >= 0 && v <= 2 ? v : 0);
    ScreenLvAsync(ApplyEngineUi);
}

void SaveEngineMain() {
    Settings st("blehid", true);
    st.SetInt("asr", s_engine.load());
}

void OnEngine(lv_event_t* e) {
    s_engine.store(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))) % 3);
    ApplyEngineUi(nullptr);
    Application::GetInstance().Schedule(SaveEngineMain);
}

void OnEngineCycle(lv_event_t*) {
    s_engine.store((s_engine.load() + 1) % 3);
    ApplyEngineUi(nullptr);
    Application::GetInstance().Schedule(SaveEngineMain);
}

void ApplyResultUi(void*) {
    if (!s_vresult) return;
    std::lock_guard<std::mutex> lk(s_result_mu);
    lv_label_set_text(s_vresult, s_result_text.c_str());
}

// 蓝牙任务里调用
void OnVoiceResult(int status, const char* text) {
    {
        std::lock_guard<std::mutex> lk(s_result_mu);
        if (status == 0) s_result_text = std::string("已输入：") + text;
        else if (status == 1) s_result_text = std::string("没听清，没输入") + (text[0] ? std::string("（") + text + "）" : "");
        else s_result_text = std::string("识别失败：") + text;
    }
    if (s_active.load()) ScreenLvAsync(ApplyResultUi);
}

void TalkStartMain() {
    auto& app = Application::GetInstance();
    app.EnsureAudioServiceRunning();
    auto& audio = app.GetAudioService();
    if (!audio.IsStarted()) {
        ESP_LOGW(TAG, "audio service not started");
        s_talking.store(false);
        return;
    }
    audio.ResetDecoder();
    ble_hid::VoiceBegin(static_cast<uint8_t>(s_engine.load()));
    audio.SetPcmCaptureSink([](std::vector<int16_t>&& pcm) { ble_hid::VoicePcm(pcm.data(), pcm.size()); });
    audio.EnableVoiceProcessing(true);
    app.SetDeviceState(kDeviceStateListening);
}

void TalkStopMain() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    audio.EnableVoiceProcessing(false);
    audio.SetPcmCaptureSink(nullptr);
    app.SetDeviceState(kDeviceStateIdle);
    ble_hid::VoiceEnd();
}

void ApplyTalkUi(void*) {
    const bool on = s_talking.load();
    if (s_ui.vbtn) {
        SetInv(s_ui.vbtn, on);
        lv_label_set_text(lv_obj_get_child(s_ui.vbtn, 0), on ? "正在听…\n松开发送" : "按住说话");
    }
    if (s_ui.vbtn2) {
        SetInv(s_ui.vbtn2, on);
        lv_label_set_text(lv_obj_get_child(s_ui.vbtn2, 0), on ? "正在听…" : "按住说话");
    }
}

void TalkStart() {
    if (s_talking.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(s_result_mu);
        s_result_text = "正在听…";
    }
    ScreenLvAsync(ApplyResultUi);
    HapticPulseIfEnabled();
    Application::GetInstance().Schedule(TalkStartMain);
    ScreenLvAsync(ApplyTalkUi);
}

void TalkStop() {
    if (!s_talking.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(s_result_mu);
        s_result_text = "识别中…";
    }
    ScreenLvAsync(ApplyResultUi);
    Application::GetInstance().Schedule(TalkStopMain);
    ScreenLvAsync(ApplyTalkUi);
}

void OnTalk(lv_event_t* e) {
    const lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) TalkStart();
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) TalkStop();
}

bool OnBootDown() {
    TalkStart();  // 侧边 BOOT 键：在本页任意子页都能按住说话
    return true;
}
bool OnBootUp() {
    TalkStop();
    return true;
}
bool OnBootEat() { return true; }

void OnEnter(lv_event_t*) { ble_hid::KeyTap(0, 0x28); }
void OnBackspace(lv_event_t*) { ble_hid::KeyTap(0, 0x2A); }

void BuildVoice(lv_obj_t* pg, int h) {
    // 上：识别结果（大）｜中：圆形「按住说话」｜下：识别方式 + 退格 + 回车
    const int card_h = 190, row_h = 60, gap = 12;
    lv_obj_t* card = Box(pg);
    lv_obj_set_pos(card, 10, 0);
    lv_obj_set_size(card, kW - 20, card_h);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    s_vresult = Label(card, "");
    lv_obj_set_width(s_vresult, kW - 52);
    lv_obj_set_height(s_vresult, card_h - 32);
    lv_label_set_long_mode(s_vresult, LV_LABEL_LONG_DOT);
    ApplyResultUi(nullptr);

    const int row_y = h - row_h;
    const int area_top = card_h + gap, area_h = row_y - gap - area_top;
    const int d = area_h < 300 ? area_h : 300;
    lv_obj_t* b = Box(pg);
    s_ui.vbtn = b;
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, (kW - d) / 2, area_top + (area_h - d) / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_4));
    lv_obj_clear_flag(b, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN));
    lv_obj_t* l = Label(b, "按住说话", s_ui.fb);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    for (auto c : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) lv_obj_add_event_cb(b, OnTalk, c, nullptr);

    const int kw = 96, sw = kW - 20 - 2 * (kw + 10);
    Segmented(pg, 10, row_y, sw, row_h, kEngineShort, 3, OnEngine, s_ui.eng);
    ApplyEngineUi(nullptr);
    lv_obj_t* bs = Btn(pg, "退格", 10 + sw + 10, row_y, kw, row_h, OnBackspace, 0, LV_EVENT_PRESSED);
    lv_obj_add_event_cb(bs, OnBackspace, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    lv_obj_add_flag(bs, LV_OBJ_FLAG_USER_4);
    lv_obj_t* enter = Btn(pg, "回车", kW - 10 - kw, row_y, kw, row_h, OnEnter, 0, LV_EVENT_PRESSED);
    lv_obj_add_flag(enter, LV_OBJ_FLAG_USER_4);
}

void ApplyVoiceHint() {
    // 还没说过话时，结果卡片里显示电脑端程序连没连上
    bool empty;
    {
        std::lock_guard<std::mutex> lk(s_result_mu);
        empty = s_result_text.empty() || s_result_text.rfind("电脑端", 0) == 0;
        if (empty)
            s_result_text = ble_hid::VoiceHostReady() ? "电脑端已连上。按住说话，松手后文字打到电脑光标处（侧边键也行）"
                                                      : "电脑端程序没连上：在电脑上运行 tools/ble_voice/eink_voice.py";
    }
    if (empty) ApplyResultUi(nullptr);
}

// ---------------------------------------------------------------- 底部触摸键：左右切换功能页

bool OnVkKey(const char* key) {
    if (!key) return false;
    const bool next = std::strcmp(key, "vk_next") == 0;
    if (!next && std::strcmp(key, "vk_prev") != 0) return false;  // 主页键照旧回桌面
    struct Arg {
        static void Run(void* p) {
            const int cur = s_kb_root ? 1 : s_page;
            const int d = p ? 1 : 3;  // +1 / -1（mod 4）
            ShowPage((cur + d) % 4);
        }
    };
    ScreenLvAsync(Arg::Run, next ? reinterpret_cast<void*>(1) : nullptr);
    return true;
}

// ---------------------------------------------------------------- 生命周期

void OnLifecycle(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        s_active.store(true);
        s_start_failed.store(false);
        ble_hid::SetStateCallback(OnBleState);
        if (!s_inhibit) {
            PowerPolicy::GetInstance().Acquire(PowerNeed::StandbyInhibit);  // 当键鼠用时别自己锁屏
            s_inhibit = true;
        }
        s_want.store(true);
        Kick();
        LVAdapterDisplay::SetTouchRawHook(RawTouch, 8);  // 触摸 8ms 轮询（esp_timer 叫醒，约 125Hz）
        ble_hid::SetVoiceResultCallback(OnVoiceResult);
        Application::GetInstance().Schedule(LoadEngineMain);
        ApplyState(nullptr);
    } else if (lv_event_get_target(e) == s_ui.scr) {
        s_active.store(false);
        TalkStop();
        ble_hid::SetVoiceResultCallback(nullptr);
        s_vresult = nullptr;
        LVAdapterDisplay::SetTouchRawHook(nullptr, 0);
        s_raw_mode = RawMode::kNone;
        s_kb_root = nullptr;  // 跟着屏幕一起删
        if (s_kb_timer) {
            lv_timer_delete(s_kb_timer);
            s_kb_timer = nullptr;
        }
        s_kb_state = nullptr;
        SetTilt(false);
        s_drag = false;
        s_want.store(false);
        Kick();
        if (s_inhibit) {
            PowerPolicy::GetInstance().Release(PowerNeed::StandbyInhibit);
            s_inhibit = false;
        }
        s_ui = Ui{};
    }
}

}  // namespace

lv_obj_t* BleHidScreen::Create() {
    s_ui = Ui{};
    s_ui.f = fontpack_lv_font_ui();
    s_ui.fb = fontpack_lv_font_get(30, 4);
    if (!s_ui.fb) s_ui.fb = s_ui.f;
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.scr = scr;
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    s_ui.top = status.height;

    // 头部一行：状态圆点 + 连接状态 | 忘记配对
    const int hy = s_ui.top + 8, hh = 50;
    s_ui.dot = Box(scr);
    lv_obj_set_size(s_ui.dot, 18, 18);
    lv_obj_set_pos(s_ui.dot, 14, hy + (hh - 18) / 2);
    lv_obj_set_style_radius(s_ui.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ui.dot, 2, 0);
    lv_obj_set_style_border_color(s_ui.dot, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_ui.dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ui.dot, LV_OPA_COVER, 0);
    s_ui.state = Label(scr, "正在打开蓝牙…");
    lv_obj_set_width(s_ui.state, kW - 44 - 130);
    lv_label_set_long_mode(s_ui.state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ui.state, LV_ALIGN_TOP_LEFT, 42, hy + 10);
    {
        lv_obj_t* more = Box(scr);  // 无框文字按钮：不常用，别抢眼
        lv_obj_set_pos(more, kW - 10 - 110, hy);
        lv_obj_set_size(more, 110, hh);
        lv_obj_add_flag(more, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(more);
        lv_obj_t* ml = Label(more, "配对…");
        lv_obj_align(ml, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_add_event_cb(more, OnMenuOpen, LV_EVENT_CLICKED, nullptr);
    }

    // 功能切换：一条分段栏
    const int ty = hy + hh + 10, th = 58;
    static const char* const kTabs[] = {"触控板", "键盘", "遥控", "语音"};
    Segmented(scr, 10, ty, kW - 20, th, kTabs, 4, OnTab, s_ui.tabs);

    const int by = ty + th + 12, bh = LV_VER_RES - by - 8;
    for (int i = 0; i < 4; ++i) {
        if (i == 1) continue;  // 键盘是横屏整页，单独建
        s_ui.pages[i] = Box(scr);
        lv_obj_set_pos(s_ui.pages[i], 0, by);
        lv_obj_set_size(s_ui.pages[i], kW, bh);
    }
    BuildPad(s_ui.pages[0], bh);
    BuildRemote(s_ui.pages[2], bh);
    BuildVoice(s_ui.pages[3], bh);
    ShowPage(s_page);

    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);
    ScreenSetIsHome(false);
    VkKeyScreenDesc desc{};
    desc.factory = BleHidScreen::Create;
    desc.on_key = OnVkKey;
    desc.on_boot_press_down = OnBootDown;
    desc.on_boot_press_up = OnBootUp;
    desc.on_boot_click = OnBootEat;
    desc.on_boot_long_press = OnBootEat;
    VkKey_AttachScreen(scr, kScreenName, desc);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(scr, OnLifecycle, LV_EVENT_SCREEN_UNLOADED, nullptr);
    ESP_LOGI(TAG, "create");
    return scr;
}

bool BleHidScreen::IsActive() { return s_active.load(); }

void BleHidScreen::SetTiltAxes(bool swap, int sx, int sy) {
    s_axis_swap = swap;
    s_axis_sx = sx < 0 ? -1 : 1;
    s_axis_sy = sy < 0 ? -1 : 1;
    ESP_LOGW(TAG, "tilt axes swap=%d sx=%d sy=%d", swap, s_axis_sx, s_axis_sy);
}

void BleHidScreen::DebugTalk(bool on) {
    if (on) TalkStart();
    else TalkStop();
}
