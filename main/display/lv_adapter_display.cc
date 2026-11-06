#include "lv_adapter_display.h"
#include "shutdown_hint_bitmap.h"
#include "clock_glyphs.h"
#include <esp_private/esp_clk.h>
#include "date_glyphs.h"
#include <ctime>
#include "display/screen/standby_screen/standby_card.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_mmap_assets.h>
#include <esp_timer.h>
#include <font_awesome.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "battery_uv_lockout.h"
#include "assistant_screen/assistant_screen.h"
#include "audio_codec.h"
#include "board.h"
#include "power_policy.h"
#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "src/misc/lv_area_private.h"
#include "src/core/lv_refr_private.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/settings/book_tap_zones.h"
#include "display_orient.h"
#include "epd_gray_aa.h"
#include "epd_i1_glyph_thin.h"
#include "haptic_feedback.h"
#include "home_screen/home_screen.h"
#include "quick_panel.h"
#include "touch_missing_screen/touch_missing_screen.h"
#include "mmap_generate_resources.h"
#include <wifi_station.h>
#include "a2ui_image.h"
#include "sd_paths.h"
#include "settings.h"
#include "SdCardManager.hpp"
#include "wallpaper_screen/wallpaper_active.h"
#include "reader/book_home_snapshot.h"

#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"
#include "fontpack_lvgl.h"

#include <cerrno>
#include <sys/stat.h>

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay* LVAdapterDisplay::instance_ = nullptr;

namespace {

#pragma pack(push, 1)
struct A2i1Header {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t reserved;
};
#pragma pack(pop)

bool ParseA2i1(const uint8_t* buf, size_t len, uint16_t* w, uint16_t* h, uint16_t* stride,
               const uint8_t** payload) {
    if (buf == nullptr || len < sizeof(A2i1Header) + 8) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const A2i1Header*>(buf);
    if (hdr->magic != A2UI_I1_MAGIC || hdr->width == 0 || hdr->height == 0 || hdr->stride == 0) {
        return false;
    }
    const size_t need =
        sizeof(A2i1Header) + 8u + static_cast<size_t>(hdr->stride) * static_cast<size_t>(hdr->height);
    if (len < need) {
        return false;
    }
    if (w) {
        *w = hdr->width;
    }
    if (h) {
        *h = hdr->height;
    }
    if (stride) {
        *stride = hdr->stride;
    }
    if (payload) {
        *payload = buf + sizeof(A2i1Header); /* palette (8) + bitmap */
    }
    return true;
}

// 优先用 SD 上由 NVS 指向的壁纸；调用方负责用 heap_caps_free 释放。
bool TryLoadShutdownA2i1FromSd(uint8_t** out_buf, size_t* out_len) {
    if (out_buf == nullptr || out_len == nullptr) {
        return false;
    }
    *out_buf = nullptr;
    *out_len = 0;

    if (!SdCardManager::GetInstance().IsMounted()) {
        ESP_LOGD(TAG, "shutdown img: SD not mounted → built-in");
        return false;
    }

    char path[192] = {};
    if (!Wallpaper_TryResolveActiveWallpaperPath(path, sizeof(path)) &&
        !StandbyCard_TryPath(path, sizeof(path))) {
        ESP_LOGD(TAG, "shutdown img: no NVS shutdown file → built-in");
        return false;
    }

    struct stat st {};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ESP_LOGD(TAG, "shutdown img: missing %s → built-in", path);
        return false;
    }
    // A2I1 头至少 magic+wh+stride+res+palette = 20
    constexpr off_t kMinBytes = 20;
    constexpr off_t kMaxBytes = 256 * 1024;
    if (st.st_size < kMinBytes || st.st_size > kMaxBytes) {
        ESP_LOGW(TAG, "shutdown img: bad size %ld → built-in", static_cast<long>(st.st_size));
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "shutdown img: fopen %s fail errno=%d → built-in", path, errno);
        return false;
    }

    const size_t need = static_cast<size_t>(st.st_size);
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        fclose(f);
        ESP_LOGW(TAG, "shutdown img: alloc %u fail → built-in", static_cast<unsigned>(need));
        return false;
    }

    const size_t nread = fread(buf, 1, need, f);
    fclose(f);
    if (nread != need) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "shutdown img: short read %u/%u → built-in", static_cast<unsigned>(nread),
                 static_cast<unsigned>(need));
        return false;
    }

    // 快速魔数校验，坏文件留给内置，避免关机白屏/花屏
    if (!(need >= 4 && buf[0] == 'A' && buf[1] == '2' && buf[2] == 'I' && buf[3] == '1')) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "shutdown img: not A2I1 %s → built-in", path);
        return false;
    }

    ESP_LOGI(TAG, "shutdown img: use SD %s (%u bytes)", path, static_cast<unsigned>(need));
    *out_buf = buf;
    *out_len = need;
    return true;
}

constexpr size_t kMaxTouchVirtualKeys = 8;
TouchVirtualKey s_vk_keys[kMaxTouchVirtualKeys];
size_t s_vk_count = 0;
TouchVirtualKeyEventCb s_vk_cb = nullptr;
void* s_vk_user = nullptr;

// long_press_ms==0：按下即 Click（旧行为），仅用 was_down 防抖。
bool s_vk_instant_was_down = false;
bool s_finger_was_down = false;

// long_press_ms>0：完整按住生命周期（无堆分配，无定时器句柄）。
struct TouchVkHoldState {
    const char* name = nullptr;
    uint16_t long_press_ms = 0;
    int64_t down_us = 0;
    bool long_emitted = false;
    bool long_consumed = false;
};
TouchVkHoldState s_vk_hold{};

// 盖板键与 LVGL 刷屏并行：feed 读 I2C + 发虚拟键；屏内点走边沿锁存再喂 LVGL。
// 1=打印 feed latch / indev deliver 详细轨迹；量产保持 0。
#ifndef LV_ADAPTER_TOUCH_TRACE
#define LV_ADAPTER_TOUCH_TRACE 0
#endif
constexpr int kTouchFeedPeriodMs = 10;
std::atomic<int> s_touch_period_ms{kTouchFeedPeriodMs};  // 蓝牙触控板时调短
std::atomic<void (*)(bool, int, int)> s_touch_raw_hook{nullptr};
std::atomic<bool> s_dbg_drag{false};  // 调试拖动期间真触摸不喂钩子
constexpr UBaseType_t kTouchFeedPriority = 5; // > LVGL(1)
constexpr uint8_t kTouchEdgeQueueDepth = 50; // LVGL 忙时 down/up 边沿积压上限
struct TouchDownEdge {
    uint16_t x = 0;
    uint16_t y = 0;
};
struct TouchFeedSnapshot {
    uint8_t live_count = 0;     // 当前是否按着（UI，非盖板键）
    uint16_t live_x = 0;
    uint16_t live_y = 0;
    TouchDownEdge down_q[kTouchEdgeQueueDepth]{}; // 按下边沿坐标环（按次交付，勿覆盖）
    uint8_t down_head = 0;      // 下一笔待交付
    uint8_t pending_down = 0;   // 未交付给 LVGL 的按下边沿
    uint8_t pending_up = 0;     // 未交付给 LVGL 的抬起边沿
    bool lvgl_down = false;     // LVGL 侧是否已处于 pressed
    bool suppress_lvgl = false; // 盖板键占用中
};
portMUX_TYPE s_touch_snap_mux = portMUX_INITIALIZER_UNLOCKED;
TouchFeedSnapshot s_touch_snap{};
int64_t s_touch_mark_us = 0;  // 最近一次触摸抬起（耗时打点：抬手 → 送屏）
TaskHandle_t s_touch_feed_task = nullptr;
volatile bool s_touch_feed_run = false;
esp_lcd_touch_handle_t s_touch_tp = nullptr;
bool s_standby_background_paused = false;

const TouchVirtualKey* HitVirtualKey(int x, int y) {
    for (size_t i = 0; i < s_vk_count; ++i) {
        if (x == s_vk_keys[i].x && y == s_vk_keys[i].y) {
            return &s_vk_keys[i];
        }
    }
    return nullptr;
}

bool EmitVkEvent(const char* name, TouchVkEvent event) {
    if (s_vk_cb == nullptr || name == nullptr) {
        return false;
    }
    // 与 BOOT 键同：可在非 LVGL 任务调用；页面跳转内部须 ScreenLvAsync，勿在此同步碰 LVGL。
    const int64_t t0 = esp_timer_get_time();
    const bool r = s_vk_cb(name, event, s_vk_user);
    const int us = static_cast<int>(esp_timer_get_time() - t0);
    if (event == TouchVkEvent::Click || event == TouchVkEvent::LongPress) {
        ESP_LOGD(TAG, "vk cb %s ev=%d ret=%d took %d us", name, static_cast<int>(event),
                 r ? 1 : 0, us);
        if (us > 20000) {
            ESP_LOGW(TAG, "vk cb %s blocked touch_feed %d us", name, us);
        }
    }
    return r;
}

void EndHoldPress(bool emit_click_if_unconsumed) {
    if (s_vk_hold.name == nullptr) {
        return;
    }
    const char* name = s_vk_hold.name;
    const bool emit_click = emit_click_if_unconsumed && !s_vk_hold.long_consumed;
    EmitVkEvent(name, TouchVkEvent::PressUp);
    if (emit_click) {
        // 可长按键短按：按下未震，松手 Click 时补震
        HapticPulseIfEnabled();
        EmitVkEvent(name, TouchVkEvent::Click);
    }
    s_vk_hold = {};
}

/** 结束一次 UI 按下（抬起或改点到盖板键）：锁存 up，避免 LVGL 忙碌时丢掉松手。 */
bool TouchFeedUiReleaseLocked() {
    if (s_touch_snap.live_count == 0) {
        return false;
    }
    s_touch_mark_us = esp_timer_get_time();
    s_touch_snap.live_count = 0;
    if (s_touch_snap.pending_up < kTouchEdgeQueueDepth) {
        s_touch_snap.pending_up++;
    }
    return true;
}

bool TouchFeedUiPressLocked(uint16_t x, uint16_t y) {
    s_touch_snap.live_count = 1;
    s_touch_snap.live_x = x;
    s_touch_snap.live_y = y;
    if (s_touch_snap.pending_down < kTouchEdgeQueueDepth) {
        const uint8_t i =
            static_cast<uint8_t>((s_touch_snap.down_head + s_touch_snap.pending_down) % kTouchEdgeQueueDepth);
        s_touch_snap.down_q[i].x = x;
        s_touch_snap.down_q[i].y = y;
        s_touch_snap.pending_down++;
    }
    return true;
}

void TouchFeedKickLvgl(const char* why, uint8_t pend_d, uint8_t pend_u, uint8_t live, bool lvgl_d,
                       uint16_t x, uint16_t y) {
#if LV_ADAPTER_TOUCH_TRACE
    ESP_LOGD(TAG, "feed latch %s pend_d=%u pend_u=%u live=%u lvgl_d=%d xy=%u,%u → kick indev", why,
             pend_d, pend_u, live, lvgl_d ? 1 : 0, x, y);
#else
    (void)why;
    (void)pend_d;
    (void)pend_u;
    (void)live;
    (void)lvgl_d;
    (void)x;
    (void)y;
#endif
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->KickTouchInput();
    }
}

// 下拉快捷面板手势：从顶部状态栏（逻辑 y < kPullZoneY）按下，向下拖过 kPullDistance
constexpr int kPullZoneY = 52;
constexpr int kPullDistance = 90;
bool s_pull_armed = false;   // 本次按下起点在状态栏
bool s_pull_fired = false;   // 已触发打开；直到抬手前不再喂 LVGL
int s_pull_start_y = 0;

/** touch_feed：按下边沿早震；盖板键逻辑；屏内锁存 down/up 边沿供 LVGL 补齐 CLICKED。 */
void TouchFeedHandleSample(uint8_t count, int x, int y) {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (auto hook = s_dbg_drag.load() ? nullptr : s_touch_raw_hook.load()) {  // 原始触摸（逻辑坐标），给蓝牙触控板用，不经 LVGL 的 30ms 节拍
        int ux = x, uy = y;
        if (count > 0) DisplayUiTouchToLogical(x, y, &ux, &uy);
        hook(count > 0, ux, uy);
    }
    if (count == 0) {
        s_pull_armed = false;
        s_pull_fired = false;
        if (s_finger_was_down) {
            ESP_LOGD(TAG, "touch up");
        }
        s_finger_was_down = false;
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        bool kick = false;
        uint8_t pend_d = 0, pend_u = 0, live = 0;
        bool lvgl_d = false;
        uint16_t lx = 0, ly = 0;
        portENTER_CRITICAL(&s_touch_snap_mux);
        s_touch_snap.suppress_lvgl = false;
        kick = TouchFeedUiReleaseLocked();
        pend_d = s_touch_snap.pending_down;
        pend_u = s_touch_snap.pending_up;
        live = s_touch_snap.live_count;
        lvgl_d = s_touch_snap.lvgl_down;
        lx = s_touch_snap.live_x;
        ly = s_touch_snap.live_y;
        portEXIT_CRITICAL(&s_touch_snap_mux);
        if (kick) {
            TouchFeedKickLvgl("ui_up", pend_d, pend_u, live, lvgl_d, lx, ly);
        }
        return;
    }

    PowerPolicy::GetInstance().NotifyUserActivity();

    if (!s_finger_was_down) {
        s_finger_was_down = true;
        const TouchVirtualKey* early_hit = HitVirtualKey(x, y);
        int ui_x = x;
        int ui_y = y;
        if (early_hit == nullptr) {
            DisplayUiTouchToLogical(x, y, &ui_x, &ui_y);
        }
        ESP_LOGD(TAG, "touch down x=%d y=%d %s", ui_x, ui_y,
                 early_hit != nullptr ? early_hit->name : "ui");
        s_pull_armed = early_hit == nullptr && ui_y < kPullZoneY && !QuickPanel::IsOpen();
        s_pull_fired = false;
        s_pull_start_y = ui_y;
        if (early_hit != nullptr) {
            HapticBeginPress();
            // 可长按键：按下不震，短按 Click / 长按达成时再震（避免连震两下）
            if (early_hit->long_press_ms == 0) {
                HapticPulseOnFingerDown();
            }
        } else {
            HapticBeginPress();
            HapticTryPulseAtUiPoint(static_cast<int16_t>(ui_x), static_cast<int16_t>(ui_y));
        }
    }

    const TouchVirtualKey* hit = HitVirtualKey(x, y);
    if (hit == nullptr) {
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        int ui_x = x;
        int ui_y = y;
        DisplayUiTouchToLogical(x, y, &ui_x, &ui_y);
        if (s_pull_fired) {
            return;  // 面板已在打开：本次按住期间不再喂 LVGL
        }
        if (s_pull_armed && ui_y - s_pull_start_y >= kPullDistance) {
            s_pull_fired = true;
            s_pull_armed = false;
            ESP_LOGI(TAG, "pull-down gesture -> quick panel");
            // 结束这次 UI 按下，避免状态栏上的控件收到点击
            bool kick_up = false;
            portENTER_CRITICAL(&s_touch_snap_mux);
            kick_up = TouchFeedUiReleaseLocked();
            portEXIT_CRITICAL(&s_touch_snap_mux);
            if (kick_up) {
                TouchFeedKickLvgl("pull_up", 0, 0, 0, false, 0, 0);
            }
            QuickPanel::RequestOpen();
            return;
        }
        const uint16_t ux = static_cast<uint16_t>(ui_x);
        const uint16_t uy = static_cast<uint16_t>(ui_y);
        bool kick = false;
        uint8_t pend_d = 0, pend_u = 0, live = 0;
        bool lvgl_d = false;
        portENTER_CRITICAL(&s_touch_snap_mux);
        s_touch_snap.suppress_lvgl = false;
        if (s_touch_snap.live_count == 0) {
            kick = TouchFeedUiPressLocked(ux, uy);
        } else {
            s_touch_snap.live_x = ux;
            s_touch_snap.live_y = uy;
        }
        pend_d = s_touch_snap.pending_down;
        pend_u = s_touch_snap.pending_up;
        live = s_touch_snap.live_count;
        lvgl_d = s_touch_snap.lvgl_down;
        portEXIT_CRITICAL(&s_touch_snap_mux);
        if (kick) {
            TouchFeedKickLvgl("ui_down", pend_d, pend_u, live, lvgl_d, ux, uy);
        }
        return;
    }

    // 盖板键：不喂 LVGL；若刚才在点 UI，补一次抬起边沿
    bool kick = false;
    uint8_t pend_d = 0, pend_u = 0, live = 0;
    bool lvgl_d = false;
    uint16_t lx = 0, ly = 0;
    portENTER_CRITICAL(&s_touch_snap_mux);
    s_touch_snap.suppress_lvgl = true;
    kick = TouchFeedUiReleaseLocked();
    pend_d = s_touch_snap.pending_down;
    pend_u = s_touch_snap.pending_up;
    live = s_touch_snap.live_count;
    lvgl_d = s_touch_snap.lvgl_down;
    lx = s_touch_snap.live_x;
    ly = s_touch_snap.live_y;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    if (kick) {
        TouchFeedKickLvgl("ui_to_vk", pend_d, pend_u, live, lvgl_d, lx, ly);
    }

    if (hit->long_press_ms == 0) {
        EndHoldPress(true);
        if (!s_vk_instant_was_down) {
            s_vk_instant_was_down = true;
            ESP_LOGI(TAG, "virtual key [%s] click", hit->name);
            EmitVkEvent(hit->name, TouchVkEvent::Click);
        }
        return;
    }

    s_vk_instant_was_down = false;

    if (s_vk_hold.name != nullptr && std::strcmp(s_vk_hold.name, hit->name) != 0) {
        EndHoldPress(true);
    }

    if (s_vk_hold.name == nullptr) {
        s_vk_hold.name = hit->name;
        s_vk_hold.long_press_ms = hit->long_press_ms;
        s_vk_hold.down_us = esp_timer_get_time();
        s_vk_hold.long_emitted = false;
        s_vk_hold.long_consumed = false;
        ESP_LOGD(TAG, "virtual key [%s] press-down (long=%ums)", hit->name,
                 static_cast<unsigned>(hit->long_press_ms));
        return;
    }

    if (!s_vk_hold.long_emitted) {
        const int64_t elapsed_ms = (esp_timer_get_time() - s_vk_hold.down_us) / 1000;
        if (elapsed_ms >= static_cast<int64_t>(s_vk_hold.long_press_ms)) {
            s_vk_hold.long_emitted = true;
            ESP_LOGD(TAG, "virtual key [%s] long-press %ums", s_vk_hold.name,
                     static_cast<unsigned>(s_vk_hold.long_press_ms));
            s_vk_hold.long_consumed = EmitVkEvent(s_vk_hold.name, TouchVkEvent::LongPress);
        }
    }
}

void TouchFeedTask(void* arg) {
    auto* tp = static_cast<esp_lcd_touch_handle_t>(arg);
    esp_lcd_touch_point_data_t pts[1] = {};
    ESP_LOGD(TAG, "touch_feed start prio=%u period=%dms", static_cast<unsigned>(kTouchFeedPriority),
             kTouchFeedPeriodMs);
    while (s_touch_feed_run) {
        uint8_t count = 0;
        // 待机 LP 已写 A5 深睡：勿再 I2C 读（CST816S 不应答 → panel_io 刷 E 级日志）
        if (tp != nullptr && !PowerPolicy::GetInstance().IsTouchAsleep()) {
            if (esp_lcd_touch_read_data(tp) == ESP_OK) {
                if (esp_lcd_touch_get_data(tp, pts, &count, 1) != ESP_OK) {
                    count = 0;
                }
            }
        }
        const int x = count > 0 ? static_cast<int>(pts[0].x) : 0;
        const int y = count > 0 ? static_cast<int>(pts[0].y) : 0;
        TouchFeedHandleSample(count, x, y);
        const int per = s_touch_period_ms.load();
        if (per >= 10) {
            vTaskDelay(pdMS_TO_TICKS(per));
        } else {
            // 节拍 10ms，比它短的周期靠 esp_timer 叫醒（蓝牙触控板要 8ms ≈ 125Hz）
            static esp_timer_handle_t s_tick = nullptr;
            static int s_tick_per = 0;
            if (!s_tick) {
                esp_timer_create_args_t a = {};
                a.callback = [](void*) {
                    if (TaskHandle_t t = s_touch_feed_task) xTaskNotifyGive(t);
                };
                a.name = "touch_tick";
                esp_timer_create(&a, &s_tick);
            }
            if (s_tick_per != per) {
                esp_timer_stop(s_tick);
                esp_timer_start_periodic(s_tick, static_cast<uint64_t>(per) * 1000);
                s_tick_per = per;
            }
            ulTaskNotifyTake(pdTRUE, 2);
            if (s_touch_period_ms.load() >= 10 && s_tick_per) {  // 退出快速模式就停掉定时器
                esp_timer_stop(s_tick);
                s_tick_per = 0;
            }
        }
    }
    s_touch_feed_task = nullptr;
    vTaskDelete(nullptr);
}

void StartTouchFeed(esp_lcd_touch_handle_t tp) {
    if (tp == nullptr || s_touch_feed_task != nullptr) {
        return;
    }
    s_touch_feed_run = true;
    BaseType_t ok = xTaskCreatePinnedToCore(TouchFeedTask, "touch_feed", 4096, tp,
                                             kTouchFeedPriority, &s_touch_feed_task, 0);
    if (ok != pdPASS) {
        s_touch_feed_run = false;
        s_touch_feed_task = nullptr;
        ESP_LOGE(TAG, "touch_feed create failed");
    }
}

void StopTouchFeed() {
    if (s_touch_feed_task == nullptr) {
        return;
    }
    s_touch_feed_run = false;
    for (int i = 0; i < 50 && s_touch_feed_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/**
 * LVGL indev：按边沿交付，而不是只读「此刻快照」。
 * 否则 LVGL 忙时整段 down→up 被折叠成一次 up，出现「第一次只震不点、第二次才切页」。
 */
esp_err_t CustomTouchRead(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t* points, uint8_t* count,
                          uint8_t max_count, void* /*user_ctx*/) {
    (void)tp;
    if (points == nullptr || count == nullptr || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t out_count = 0;
    uint16_t out_x = 0;
    uint16_t out_y = 0;
    const char* why = "idle";
    uint8_t pend_d = 0, pend_u = 0, live = 0;
    bool lvgl_d_before = false;

    portENTER_CRITICAL(&s_touch_snap_mux);
    auto& s = s_touch_snap;
    pend_d = s.pending_down;
    pend_u = s.pending_up;
    live = s.live_count;
    lvgl_d_before = s.lvgl_down;
    if (s.lvgl_down) {
        if (s.pending_up > 0) {
            s.pending_up--;
            s.lvgl_down = false;
            out_count = 0;
            why = "deliver_up(pend)";
        } else if (s.live_count > 0 && !s.suppress_lvgl) {
            out_count = 1;
            out_x = s.live_x;
            out_y = s.live_y;
            why = "hold";
        } else {
            // 已松开但边沿被吞：补一次 up，避免粘住 PRESSED
            s.lvgl_down = false;
            out_count = 0;
            why = "deliver_up(force)";
        }
    } else if (s.pending_down > 0) {
        out_x = s.down_q[s.down_head].x;
        out_y = s.down_q[s.down_head].y;
        s.down_head = static_cast<uint8_t>((s.down_head + 1) % kTouchEdgeQueueDepth);
        s.pending_down--;
        s.lvgl_down = true;
        out_count = 1;
        why = "deliver_down(pend)";
    } else if (s.live_count > 0 && !s.suppress_lvgl) {
        s.lvgl_down = true;
        out_count = 1;
        out_x = s.live_x;
        out_y = s.live_y;
        why = "deliver_down(live)";
    } else {
        out_count = 0;
        why = "idle";
    }
    const uint8_t pend_d_after = s.pending_down;
    const uint8_t pend_u_after = s.pending_up;
    const bool lvgl_d_after = s.lvgl_down;
    // down 已交付但 up 仍锁存：必须再踢一次 indev，否则 LVGL 可能不再读，CLICKED 永不到
    const bool kick_again = (out_count == 1 && pend_u_after > 0) || (out_count == 0 && pend_d_after > 0);
    portEXIT_CRITICAL(&s_touch_snap_mux);

    // 只打边沿交付，避免 hold/idle 刷屏；对照 feed latch 时间戳即可定位
#if LV_ADAPTER_TOUCH_TRACE
    if (why[0] == 'd') { // deliver_*
        ESP_LOGI(TAG,
                 "indev %s out=%u xy=%u,%u before(pend_d=%u pend_u=%u live=%u lvgl_d=%d) "
                 "after(pend_d=%u pend_u=%u lvgl_d=%d)%s",
                 why, out_count, out_x, out_y, pend_d, pend_u, live, lvgl_d_before ? 1 : 0, pend_d_after,
                 pend_u_after, lvgl_d_after ? 1 : 0, kick_again ? " → kick again" : "");
    }
#else
    (void)why;
    (void)pend_d;
    (void)pend_u;
    (void)live;
    (void)lvgl_d_before;
    (void)pend_d_after;
    (void)pend_u_after;
    (void)lvgl_d_after;
#endif

    if (kick_again) {
        if (auto* disp = LVAdapterDisplay::Instance()) {
            disp->KickTouchInput();
        }
    }

    if (out_count == 0) {
        *count = 0;
        return ESP_OK;
    }
    *count = 1;
    points[0] = {};
    points[0].x = out_x;
    points[0].y = out_y;
    return ESP_OK;
}

}  // namespace

/*
 * LVGL I1 / HTILED：1=白、0=黑，与面板 VRAM（0xFF=白）一致，写屏不取反。
 * 全刷：fast LUT → 0x26=0x24=新图 → 0x22=FULL → 0x20 → BUSY（旧 0xC7 / 优化 0xC4）
 * 局刷：partial LUT → 边框0x80 → 整帧只写 0x24 → 0x22=PARTIAL → 0x20（旧 0xCF / 优化 0xCC）
 * 阅读抗锯齿：局刷/偶发 FULL 铺 BW 底 → Overlay 掩码 → 灰 LUT + 0xCC
 * SSD1677_EPD_OPT_PATH=0 走旧流程；=1 启用优化档（含 0x37/0x3F）。
 * 每 15 次局刷强制一次全刷；开机/关机走全刷。
 *
 * 触摸：PARTIAL 启动后立刻 flush_ready；下次写屏用 TryFinish（不等 BUSY），忙则 coalesce。
 * coalesce 后若无后续 flush（如打开书进度条→正文），BUSY 结束后由 esp_timer 补刷 work_fb。
 * 盖板虚拟键：touch_feed 并行采点/震动/发事件（跳转内部 lv_async）。
 * 屏内：无锁对照 HapticAttachClick 区域表早震；feed 锁存 down/up 供 indev。
 */
static constexpr uint8_t kLvglWhite = 0xFF;
static constexpr int kPartialDirtyPadPx = 24;
/** 连续局刷达到该次数后，下一次改走全刷清残影；0 = 不自动全刷（设置→屏幕可调，存 NVS） */
static constexpr int kDefaultPartialsPerFull = 15;
static std::atomic<int> s_partials_per_full{kDefaultPartialsPerFull};
static constexpr const char* kEpdNvsNs = "epd";
static constexpr const char* kEpdNvsFullEvery = "full_every";

static uint32_t PartialsPerFull() {
    const int n = s_partials_per_full.load(std::memory_order_relaxed);
    return n <= 0 ? UINT32_MAX : static_cast<uint32_t>(n);
}

struct EpdFlushCtx {
    lv_display_t* disp = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    SemaphoreHandle_t done_sem = nullptr;
    uint8_t* last_fb = nullptr;
    uint8_t* work_fb = nullptr;
    uint8_t* gray_scratch = nullptr;  // Overlay 单平面 scratch（LSB/MSB 分两次打包）
    size_t fb_size = 0;
    int panel_w = 0;
    int panel_h = 0;
    bool has_last = false;
    uint32_t partial_refresh_count = 0;
    uint32_t partial_weight_milli = 0;    // 局刷按变化面积折算的累计量，满 1000 记 1 次
    int64_t last_partial_us = 0;          // 耗时打点：上次局刷开始时刻
    bool defer_boot_paint = true;  // SetupUI 完成前只攒帧不上屏
    bool boot_fb_ready = false;
    bool defer_standby_paint = false;     // 进待机 settle：只攒帧，Park 再全刷
    volatile bool suppress_lvgl_notify = false;
    volatile bool freeze_updates = false; // 关机画已落墨 / 待机 park：丢弃后续 LVGL flush
    bool standby_frozen = false;          // 待机冻结 flush（无 Deep Sleep）
    bool force_next_full = false;         // 下一帧强制全刷（退待机后画回底层）
    bool force_full_c7 = false;           // 强制全刷走 STANDBY 枚举（进/出待机）；否则 FULL（二者均为 0xC7）
    bool force_dual_plane_partial = false; // 下一帧 0x26=旧 0x24=新 局刷（AA 开/关 seed）
    /**
     * 局刷已启动、BUSY 尚未结束：flush 已提前 flush_ready，LVGL 可继续读触摸。
     * 下次写屏前 Drain（不等 Sync）。
     */
    bool refresh_inflight = false;
    bool coalesce_pending = false; // BUSY 时合并脏帧，flush 立刻返回好让 indev 跑
    volatile bool absorb_flush = false;  // 看图一次刷：LVGL 刷屏只进帧缓冲
    volatile bool gray_hold = false;     // 看图保灰：状态栏的重画先攒着
    int gray_hold_h = 0;
    bool coalesce_kick_armed = false; // 已挂 esp_timer 等待 BUSY 后补刷 coalesce
    esp_timer_handle_t coalesce_timer = nullptr;
};

namespace {

void EpdAreaRounder(lv_area_t* area, void* user_data) {
    (void)user_data;
    // 扩 invalidate：让 LVGL 把旧字/AA 边缘也重绘成白，局刷 CURR 才有黑→白差分
    area->x1 -= kPartialDirtyPadPx;
    area->y1 -= kPartialDirtyPadPx / 2;
    area->x2 += kPartialDirtyPadPx;
    area->y2 += kPartialDirtyPadPx / 2;
    if (area->x1 < 0) {
        area->x1 = 0;
    }
    if (area->y1 < 0) {
        area->y1 = 0;
    }
    const int log_w = DisplayUiLogW();
    const int log_h = DisplayUiLogH();
    if (log_w > 0 && area->x2 >= log_w) {
        area->x2 = log_w - 1;
    }
    if (log_h > 0 && area->y2 >= log_h) {
        area->y2 = log_h - 1;
    }
    area->x1 = (area->x1 >> 3) << 3;
    area->x2 = ((area->x2 >> 3) << 3) + 7;
    if (log_w > 0 && area->x2 >= log_w) {
        area->x2 = log_w - 1;
    }
}

bool EpdRefreshDoneCb(const esp_lcd_panel_handle_t handle, const void* edata, void* user_data) {
    (void)handle;
    (void)edata;
    auto* ctx = static_cast<EpdFlushCtx*>(user_data);
    if (!ctx) {
        return false;
    }

    if (ctx->suppress_lvgl_notify) {
        BaseType_t hp_task = pdFALSE;
        if (ctx->done_sem) {
            xSemaphoreGiveFromISR(ctx->done_sem, &hp_task);
        }
        if (hp_task == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return false;
    }

    return esp_lv_adapter_display_notify_frame_done_from_isr(ctx->disp);
}

static void AlignArea(int x_start, int y_start, int x_end, int y_end, int* x_draw, int* y_draw,
                      int* w_draw, int* h_draw) {
    const int len_x = x_end - x_start;
    const int len_y = y_end - y_start;
    *x_draw = x_start - (x_start % 8);
    *y_draw = y_start;
    *w_draw = ((len_x + 7) / 8) * 8;
    *h_draw = len_y;
}

// 扩脏区：边距盖住 AA/旧字边缘；再按 last⊕work 把真正变过的字节并进去
static void ExpandPartialDirty(EpdFlushCtx* ctx, int* x_draw, int* y_draw, int* w_draw, int* h_draw) {
    int x0 = *x_draw - kPartialDirtyPadPx;
    int y0 = *y_draw - kPartialDirtyPadPx;
    int x1 = *x_draw + *w_draw + kPartialDirtyPadPx;
    int y1 = *y_draw + *h_draw + kPartialDirtyPadPx;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ctx->panel_w) {
        x1 = ctx->panel_w;
    }
    if (y1 > ctx->panel_h) {
        y1 = ctx->panel_h;
    }
    x0 = (x0 >> 3) << 3;
    x1 = ((x1 + 7) >> 3) << 3;
    if (x1 > ctx->panel_w) {
        x1 = ctx->panel_w;
    }

    const int fb_stride = ctx->panel_w / 8;
    int ch_x0 = x1;
    int ch_x1 = x0;
    int ch_y0 = y1;
    int ch_y1 = y0;
    bool any = false;
    for (int y = y0; y < y1; y++) {
        const uint8_t* a = ctx->last_fb + y * fb_stride + (x0 / 8);
        const uint8_t* b = ctx->work_fb + y * fb_stride + (x0 / 8);
        const int nbytes = (x1 - x0) / 8;
        for (int i = 0; i < nbytes; i++) {
            if (a[i] == b[i]) {
                continue;
            }
            any = true;
            const int bx0 = x0 + i * 8;
            const int bx1 = bx0 + 8;
            if (bx0 < ch_x0) {
                ch_x0 = bx0;
            }
            if (bx1 > ch_x1) {
                ch_x1 = bx1;
            }
            if (y < ch_y0) {
                ch_y0 = y;
            }
            if (y + 1 > ch_y1) {
                ch_y1 = y + 1;
            }
        }
    }

    if (!any) {
        *w_draw = 0;
        *h_draw = 0;
        return;
    }

    // 变化核再扩一圈，避免字缘灰度抖到脏区外
    ch_x0 -= kPartialDirtyPadPx;
    ch_y0 -= kPartialDirtyPadPx / 2;
    ch_x1 += kPartialDirtyPadPx;
    ch_y1 += kPartialDirtyPadPx / 2;
    if (ch_x0 < 0) {
        ch_x0 = 0;
    }
    if (ch_y0 < 0) {
        ch_y0 = 0;
    }
    if (ch_x1 > ctx->panel_w) {
        ch_x1 = ctx->panel_w;
    }
    if (ch_y1 > ctx->panel_h) {
        ch_y1 = ctx->panel_h;
    }
    ch_x0 = (ch_x0 >> 3) << 3;
    ch_x1 = ((ch_x1 + 7) >> 3) << 3;
    if (ch_x1 > ctx->panel_w) {
        ch_x1 = ctx->panel_w;
    }

    *x_draw = ch_x0;
    *y_draw = ch_y0;
    *w_draw = ch_x1 - ch_x0;
    *h_draw = ch_y1 - ch_y0;
}

// ---------------------------------------------------------------- 按下即反色
// 手指按下：被按的控件在送屏时整块反色，约 0.1 秒后局刷出来，让人马上知道点上了；
// 松手：清掉反色并重画该区域，和点击引起的界面变化合成同一次刷新。
// 反色只在送屏这一步做（逻辑→面板旋转之后），不改 LVGL 的画面，所以不用每个页面改样式。
static lv_area_t s_inv_area;               // 逻辑坐标
static volatile bool s_inv_on = false;
static volatile bool s_press_flash = false;  // 下一次局刷是「按下反色」：用短波形，少占屏幕忙时
// 短波形帧数；0 = 关（按下反色也用普通局刷）。
// 短波形会让被按的那块只变成深灰，但芯片按「已经全黑」记账；下一屏这里要显示黑字时只补 4 帧，字发灰，看着像残影。
static int s_press_tp0 = 0;
static int s_norm_tp0 = 13, s_norm_tp1 = 4;
static void PressInvApply(uint8_t* dst, int panel_w, int panel_h, int x, int y, int w, int h) {
    if (!s_inv_on) return;
    const int ix1 = std::max<int>(x, s_inv_area.x1), iy1 = std::max<int>(y, s_inv_area.y1);
    const int ix2 = std::min<int>(x + w - 1, s_inv_area.x2), iy2 = std::min<int>(y + h - 1, s_inv_area.y2);
    if (ix1 > ix2 || iy1 > iy2) return;
    int pa = 0, pb = 0, pc = 0, pd = 0;
    DisplayUiLogicalToPanel(ix1, iy1, &pa, &pb);
    DisplayUiLogicalToPanel(ix2, iy2, &pc, &pd);
    const int px1 = std::max(0, std::min(pa, pc)), px2 = std::min(panel_w - 1, std::max(pa, pc));
    const int py1 = std::max(0, std::min(pb, pd)), py2 = std::min(panel_h - 1, std::max(pb, pd));
    if (px1 > px2 || py1 > py2) return;
    const int stride = panel_w / 8;
    const int b1 = px1 >> 3, b2 = px2 >> 3;
    const uint8_t m1 = static_cast<uint8_t>(0xFF >> (px1 & 7));
    const uint8_t m2 = static_cast<uint8_t>(0xFF << (7 - (px2 & 7)));
    for (int py = py1; py <= py2; ++py) {
        uint8_t* row = dst + py * stride;
        if (b1 == b2) {
            row[b1] ^= static_cast<uint8_t>(m1 & m2);
            continue;
        }
        row[b1] ^= m1;
        for (int b = b1 + 1; b < b2; ++b) row[b] ^= 0xFF;
        row[b2] ^= m2;
    }
}

static int64_t s_overlay_us = 0;  // 本帧 OverlayDirty 累计耗时（打点）
static int64_t s_flush_enter_us = 0;  // 本帧第一次进 flush 的时刻（打点）
static void OverlayDirtyImpl(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                             int y_draw, int w_draw, int h_draw);
static void OverlayDirty(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                         int y_draw, int w_draw, int h_draw) {
    const int64_t t0 = esp_timer_get_time();
    if (s_flush_enter_us == 0) s_flush_enter_us = t0;
    OverlayDirtyImpl(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
    PressInvApply(dst, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
    s_overlay_us += esp_timer_get_time() - t0;
}
static void OverlayDirtySlow(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                             int y_draw, int w_draw, int h_draw);

/**
 * 竖屏快速路径：逻辑 (lx,ly) → 面板 (px=ly, py=ph-1-lx)。按 8×8 位块转置，一次处理 64 像素。
 * 原来逐像素调函数转坐标，整屏 38 万像素要 560ms（翻页、点按钮慢的主要原因），这里约 10ms。
 */
static void OverlayDirtyPortraitFast(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                                     int y_draw, int w_draw, int h_draw) {
    const int src_stride = (DisplayUiLogW() + 7) / 8;
    const int dst_stride = panel_w / 8;
    const int x_end = x_draw + w_draw, y_end = y_draw + h_draw;
    const int ya = (y_draw + 7) & ~7, yb = y_end & ~7;  // 8 对齐的行段
    const int xa = (x_draw + 7) & ~7, xb = x_end & ~7;
    if (ya >= yb || xa >= xb) {
        OverlayDirtySlow(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
        return;
    }
    // 外层按逻辑列块（= 8 条面板行），内层沿逻辑行走：写面板是顺序的（PSRAM 缓存友好）
    for (int lx0 = xa; lx0 < xb; lx0 += 8) {
        uint8_t* drow[8];
        for (int c = 0; c < 8; ++c) drow[c] = dst + (panel_h - 1 - (lx0 + c)) * dst_stride;
        const uint8_t* scol = color_map + (lx0 >> 3);
        for (int ly0 = ya; ly0 < yb; ly0 += 8) {
            const uint8_t* sp = scol + ly0 * src_stride;
            // 8×8 位矩阵转置（Hacker's Delight transpose8）
            uint32_t x = (static_cast<uint32_t>(sp[0]) << 24) | (static_cast<uint32_t>(sp[src_stride]) << 16) |
                         (static_cast<uint32_t>(sp[2 * src_stride]) << 8) | sp[3 * src_stride];
            uint32_t y = (static_cast<uint32_t>(sp[4 * src_stride]) << 24) |
                         (static_cast<uint32_t>(sp[5 * src_stride]) << 16) |
                         (static_cast<uint32_t>(sp[6 * src_stride]) << 8) | sp[7 * src_stride];
            uint32_t t = (x ^ (x >> 7)) & 0x00AA00AA; x = x ^ t ^ (t << 7);
            t = (y ^ (y >> 7)) & 0x00AA00AA; y = y ^ t ^ (t << 7);
            t = (x ^ (x >> 14)) & 0x0000CCCC; x = x ^ t ^ (t << 14);
            t = (y ^ (y >> 14)) & 0x0000CCCC; y = y ^ t ^ (t << 14);
            t = (x & 0xF0F0F0F0) | ((y >> 4) & 0x0F0F0F0F);
            y = ((x << 4) & 0xF0F0F0F0) | (y & 0x0F0F0F0F);
            x = t;
            const int db = ly0 >> 3;
            drow[0][db] = static_cast<uint8_t>(x >> 24); drow[1][db] = static_cast<uint8_t>(x >> 16);
            drow[2][db] = static_cast<uint8_t>(x >> 8);  drow[3][db] = static_cast<uint8_t>(x);
            drow[4][db] = static_cast<uint8_t>(y >> 24); drow[5][db] = static_cast<uint8_t>(y >> 16);
            drow[6][db] = static_cast<uint8_t>(y >> 8);  drow[7][db] = static_cast<uint8_t>(y);
        }
    }
    // 不对齐的边：逐像素
    if (y_draw < ya) OverlayDirtySlow(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, ya - y_draw);
    if (yb < y_end) OverlayDirtySlow(dst, color_map, panel_w, panel_h, x_draw, yb, w_draw, y_end - yb);
    if (x_draw < xa) OverlayDirtySlow(dst, color_map, panel_w, panel_h, x_draw, ya, xa - x_draw, yb - ya);
    if (xb < x_end) OverlayDirtySlow(dst, color_map, panel_w, panel_h, xb, ya, x_end - xb, yb - ya);
}

static void OverlayDirtyImpl(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                             int y_draw, int w_draw, int h_draw) {
    if (DisplayUiGetOrient() == kDisplayUiPortrait && DisplayUiLogW() == panel_h && (panel_w & 7) == 0) {
        OverlayDirtyPortraitFast(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
        return;
    }
    OverlayDirtySlow(dst, color_map, panel_w, panel_h, x_draw, y_draw, w_draw, h_draw);
}

static void OverlayDirtySlow(uint8_t* dst, const uint8_t* color_map, int panel_w, int panel_h, int x_draw,
                             int y_draw, int w_draw, int h_draw) {
    const int log_w = DisplayUiLogW();
    const int src_stride = (log_w + 7) / 8;
    const int dst_stride = panel_w / 8;
    if (DisplayUiGetOrient() == kDisplayUiLandLeft && src_stride == dst_stride) {
        const int row_bytes = w_draw / 8;
        for (int row = 0; row < h_draw; row++) {
            const int y = y_draw + row;
            memcpy(dst + y * dst_stride + x_draw / 8, color_map + y * src_stride + x_draw / 8,
                   static_cast<size_t>(row_bytes));
        }
        return;
    }
    for (int ly = y_draw; ly < y_draw + h_draw; ly++) {
        for (int lx = x_draw; lx < x_draw + w_draw; lx++) {
            const bool white =
                (color_map[ly * src_stride + (lx >> 3)] & static_cast<uint8_t>(0x80 >> (lx & 7))) != 0;
            int px = 0;
            int py = 0;
            DisplayUiLogicalToPanel(lx, ly, &px, &py);
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
    }
}

static lv_display_t* s_press_disp = nullptr;
static esp_lcd_panel_handle_t s_epd_panel = nullptr;
static volatile bool* s_press_gray_hold = nullptr;  // 看图出灰期间不反色
static lv_timer_t* s_press_timer = nullptr;
static int64_t s_press_since_us = 0;

static void PressInvClear() {
    if (!s_inv_on) return;
    s_inv_on = false;
    if (s_press_disp) lv_inv_area(s_press_disp, &s_inv_area);
    if (s_press_timer) lv_timer_pause(s_press_timer);
}

/** 拖动/滚动开始、松手漏报、按太久（5 秒）都收掉反色 */
static void PressTimerCb(lv_timer_t* t) {
    lv_indev_t* indev = static_cast<lv_indev_t*>(lv_timer_get_user_data(t));
    if (!s_inv_on) {
        lv_timer_pause(t);
        return;
    }
    if (lv_indev_get_scroll_obj(indev) != nullptr || lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED ||
        esp_timer_get_time() - s_press_since_us > 5000000) {
        PressInvClear();
    }
}

static void OnPressIndevEvent(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED) {
        PressInvClear();
        return;
    }
    if (code != LV_EVENT_PRESSED || s_press_disp == nullptr) return;
    if (s_press_gray_hold && *s_press_gray_hold) return;
    lv_obj_t* o = static_cast<lv_obj_t*>(lv_event_get_param(e));
    // 子控件没挂回调、事件冒泡给父控件的：反色父控件（比如书架格子里的封面图）
    while (o && lv_obj_get_event_count(o) == 0 && lv_obj_has_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE)) {
        o = lv_obj_get_parent(o);
    }
    if (o == nullptr || lv_obj_get_parent(o) == nullptr || lv_obj_get_event_count(o) == 0) return;
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_4)) return;  // 页面可用 USER_4 关掉反色
    if (lv_obj_check_type(o, &lv_slider_class)) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    // 被滚动容器裁掉的部分不反色
    for (lv_obj_t* p = lv_obj_get_parent(o); p != nullptr; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        lv_obj_get_coords(p, &pa);
        if (!lv_area_intersect(&a, &a, &pa)) return;
    }
    const int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    const int32_t scr = lv_display_get_horizontal_resolution(s_press_disp) *
                        lv_display_get_vertical_resolution(s_press_disp);
    if (w < 12 || h < 12 || w * h > scr * 35 / 100) return;  // 整屏点按区（阅读页、看图）不反色
    if (s_inv_on) lv_inv_area(s_press_disp, &s_inv_area);
    s_inv_area = a;
    s_inv_on = true;
    s_press_flash = true;
    s_press_since_us = esp_timer_get_time();
    lv_inv_area(s_press_disp, &a);
    lv_refr_now(s_press_disp);  // 不等下一个刷新周期
    if (s_press_timer == nullptr) {
        s_press_timer = lv_timer_create(PressTimerCb, 50, lv_event_get_target(e));
    }
    lv_timer_resume(s_press_timer);
}

static esp_err_t WaitRefreshDone(EpdFlushCtx* ctx, const char* tag) {
    if (xSemaphoreTake(ctx->done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s timeout", tag);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/** 局刷：prepare → 整帧只写 0x24=新图（不写 0x26）→ 0xCF。依赖面板里 0x26 仍是上一帧。 */
static esp_err_t DrawPartialDirty(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel, const uint8_t* prev_fb,
                                  const uint8_t* curr_fb, int x_draw, int y_draw, int w_draw,
                                  int h_draw) {
    (void)x_draw;
    (void)y_draw;
    (void)w_draw;
    (void)h_draw;
    if (ctx == nullptr || panel == nullptr || curr_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // 只有按下反色这一块在变：用短波形刷（变黑不用压到最黑，松手时普通局刷会恢复）
    bool quick = false;
    if (s_press_flash) {
        s_press_flash = false;
        const int inv = lv_area_get_width(&s_inv_area) * lv_area_get_height(&s_inv_area);
        quick = s_press_tp0 > 0 && s_inv_on && w_draw * h_draw <= std::max(inv * 3, 160 * 160);  // 送屏区域会按 8/16 像素取整放大
        if (quick) epaper_panel_set_partial_timing(panel, s_press_tp0, 0, 0);
        ESP_LOGD(TAG, "press flash: dirty %dx%d inv=%d quick=%d", w_draw, h_draw, inv, quick ? 1 : 0);
    }
    // curr_fb 在 PSRAM：draw_bitmap 内再拷到内部 DMA bounce，勿再经 tx_* 中转
    ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(panel), TAG, "partial prepare");
    // 局刷按「旧图 0x26 vs 新图 0x24」决定每个像素怎么驱动。0xCF（刷完掉电）时芯片会自己把新图转成旧图；
    // 保持上电（0xCC/0x0C）时不会，旧图停在更早的画面上，没变化的地方被当成「没变」不驱动，
    // 上一屏弹过又收起的提示就会留在屏上。所以这时自己把上一帧写进 0x26（多 48KB，约 40ms）。
    if (prev_fb != nullptr) {
        epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, prev_fb), TAG,
                            "partial prev 0x26");
    }
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, curr_fb), TAG,
        "partial curr 0x24");
    epaper_panel_set_refresh_mode(panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    const esp_err_t err = epaper_panel_refresh_screen(panel);
    if (quick) epaper_panel_set_partial_timing(panel, s_norm_tp0, s_norm_tp1, 0);  // 下一次局刷换回正常波形
    return err;
}

/** 写 0x26=prev、0x24=curr（不激活）。 */
static esp_err_t WriteBothPlanes(EpdFlushCtx* ctx, const uint8_t* prev_fb, const uint8_t* curr_fb,
                                 const char* wait_tag) {
    if (ctx == nullptr || ctx->panel == nullptr || prev_fb == nullptr || curr_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    esp_err_t err =
        esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, prev_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x26: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
    err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, curr_fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x24: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/**
 * AA 用差分局刷底：显式 0x26=旧、0x24=新 → partial LUT + 0xCF，同步等 BUSY。
 */
static esp_err_t PartialBwBasePrevCurr(EpdFlushCtx* ctx, const uint8_t* prev_fb, const uint8_t* curr_fb,
                                       const char* wait_tag) {
    ESP_RETURN_ON_ERROR(epaper_panel_prepare_for_partial(ctx->panel), TAG, "aa partial prepare");
    esp_err_t err = WriteBothPlanes(ctx, prev_fb, curr_fb, wait_tag);
    if (err != ESP_OK) {
        return err;
    }
    epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s partial act: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    return WaitRefreshDone(ctx, wait_tag);
}

/** 全刷：0x26/0x24 同写新图；mode 选 FULL/STANDBY（均为 0xC7）。 */
static esp_err_t FullRefreshBothSame(EpdFlushCtx* ctx, const uint8_t* curr_fb, const char* wait_tag,
                                     esp_lcd_ssd1677_refresh_mode_t mode = SSD1677_EPAPER_REFRESH_FULL) {
    esp_err_t err = WriteBothPlanes(ctx, curr_fb, curr_fb, wait_tag);
    if (err != ESP_OK) {
        return err;
    }
    epaper_panel_set_refresh_mode(ctx->panel, mode);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s refresh: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/** Overlay 灰步：LSB/MSB 分两次打包同一 scratch → 0xCC → BW 写回双平面。 */
static esp_err_t GrayOverlayAfterBwBase(EpdFlushCtx* ctx, const char* wait_tag) {
    if (ctx->gray_scratch == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    epd_gray_aa_pack_overlay_plane(ctx->gray_scratch, 0);
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
    esp_err_t err =
        esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x24 lsb: %s", wait_tag, esp_err_to_name(err));
        return err;
    }
    epd_gray_aa_pack_overlay_plane(ctx->gray_scratch, 1);
    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s 0x26 msb: %s", wait_tag, esp_err_to_name(err));
        return err;
    }

    epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_GRAY);
    err = epaper_panel_refresh_screen(ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s gray refresh: %s", wait_tag, esp_err_to_name(err));
        return err;
    }

    // cleanup：面板 RAM 回到 BW，下一帧 0x26 才有正确「旧值」
    WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, wait_tag);
    return ESP_OK;
}

/**
 * 阅读抗锯齿：
 * - force_full_base / 无 last：C4 铺 BW 底（进阅读首帧、周期清残影）
 * - 否则：0x26=last 0x24=work 局刷换底 → Overlay 灰 0xCC
 */
static esp_err_t GrayRefreshFromWorkFb(EpdFlushCtx* ctx, const char* wait_tag, bool force_full_base) {
    if (ctx == nullptr || ctx->panel == nullptr || ctx->work_fb == nullptr ||
        ctx->gray_scratch == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool use_full_base = force_full_base || !ctx->has_last || ctx->last_fb == nullptr;
    esp_err_t err;
    if (use_full_base) {
        ESP_LOGD(TAG, "%s BW base FULL 0xC7", wait_tag);
        err = FullRefreshBothSame(ctx, ctx->work_fb, wait_tag, SSD1677_EPAPER_REFRESH_FULL);
    } else {
        ESP_LOGD(TAG, "%s BW base PARTIAL 0x26=old 0x24=new", wait_tag);
        err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, wait_tag);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = GrayOverlayAfterBwBase(ctx, wait_tag);
    if (err != ESP_OK) {
        return err;
    }

    // Overlay 末已 WriteBothPlanes(work,work) 重铺 0x26/0x24，退出 AA 后可直接局刷，勿再逼 C4
    if (!use_full_base) {
        ctx->partial_refresh_count++;
    } else {
        ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
    }
    ESP_LOGD(TAG, "%s GRAY overlay+0xCC done (base=%s)", wait_tag, use_full_base ? "FULL" : "PARTIAL");
    return ESP_OK;
}

static bool WantReaderGrayAa(void) {
    // LVGL 任务栈在 PSRAM，禁止 EnsureLoaded/NVS（会 cache_utils assert）。
    // epd_i1_glyph_aa_enabled 仅在正文沉浸态由 BookReaderPrefsSyncGlyphAa 置位。
    return epd_i1_glyph_aa_enabled != 0;
}

static int64_t s_refr_start_us = 0;   // 这一帧 LVGL 开始渲染的时刻（耗时打点）

static void OnLvglRefrStart(lv_event_t* e) {
    (void)e;
    s_refr_start_us = esp_timer_get_time();
    if (WantReaderGrayAa()) {
        epd_gray_aa_begin_frame();
    }
}

/** 排空上一次异步局刷（等 BUSY，不做 Sync）。 */
static void DrainRefreshIfNeeded(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->refresh_inflight) {
        return;
    }
    (void)WaitRefreshDone(ctx, "drain");
    ctx->refresh_inflight = false;
    ctx->suppress_lvgl_notify = false;
}

/** 非阻塞：BUSY 未结束返回 false（调用方 coalesce + flush_ready，勿堵 indev）。 */
static bool TryFinishInflightRefresh(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->refresh_inflight) {
        return true;
    }
    if (xSemaphoreTake(ctx->done_sem, 0) != pdTRUE) {
        return false;
    }
    ctx->refresh_inflight = false;
    ctx->suppress_lvgl_notify = false;
    return true;
}

static void ArmCoalesceKick(EpdFlushCtx* ctx);

/** 全刷计数按变化量折算：变化字节达到 1/4 屏记 1 次，状态栏图标/时钟这类小变化只记零头。 */
static void AccountPartial(EpdFlushCtx* ctx, int w_draw, int h_draw) {
    // 用真正变化的字节数（8 像素一组）而不是外接矩形：时钟+角标这种相距远的小变化外接框很大
    size_t changed = 0;
    {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(ctx->last_fb);
        const uint32_t* b = reinterpret_cast<const uint32_t*>(ctx->work_fb);
        const size_t n = ctx->fb_size / 4;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t x = a[i] ^ b[i];
            if (x == 0) continue;
            changed += ((x & 0xFFu) != 0) + ((x & 0xFF00u) != 0) + ((x & 0xFF0000u) != 0) + ((x >> 24) != 0);
        }
    }
    const size_t quarter = ctx->fb_size / 4;
    uint32_t milli = quarter ? static_cast<uint32_t>(changed * 1000 / quarter) : 1000;
    if (milli > 1000) milli = 1000;
    if (milli < 10) milli = 10;
    ctx->partial_weight_milli += milli;
    ctx->partial_refresh_count += ctx->partial_weight_milli / 1000;
    ctx->partial_weight_milli %= 1000;
    const int64_t now = esp_timer_get_time();
    const uint32_t every = PartialsPerFull();
    {
        const int render_ms = s_refr_start_us ? static_cast<int>((now - s_refr_start_us) / 1000) : -1;
        const int touch_ms = s_touch_mark_us && now - s_touch_mark_us < 10000000
                                 ? static_cast<int>((now - s_touch_mark_us) / 1000) : -1;
        const int lv_ms = s_refr_start_us && s_flush_enter_us ? static_cast<int>((s_flush_enter_us - s_refr_start_us) / 1000) : -1;
        ESP_LOGD(TAG, "epd timing: refr->send %dms (lvgl %dms, rotate %dms) touch->send %dms cpu=%dMHz", render_ms,
                 lv_ms, static_cast<int>(s_overlay_us / 1000), touch_ms, static_cast<int>(esp_clk_cpu_freq() / 1000000));
        s_refr_start_us = 0;
        s_flush_enter_us = 0;
        s_overlay_us = 0;
    }
    ESP_LOGI(TAG, "epd partial %dx%d chg=%uB w=%u -> %u.%03u/%u prev_busy=%ums prev22=%02X analog=%d gap=%dms", w_draw, h_draw,
             (unsigned)changed, (unsigned)milli, (unsigned)ctx->partial_refresh_count, (unsigned)ctx->partial_weight_milli,
             (unsigned)(every == UINT32_MAX ? 0 : every), (unsigned)epaper_panel_last_busy_ms(ctx->panel),
             epaper_panel_last_ctrl2(ctx->panel), epaper_panel_analog_on(ctx->panel) ? 1 : 0,
             ctx->last_partial_us ? (int)((now - ctx->last_partial_us) / 1000) : -1);
    ctx->last_partial_us = now;
}

/** BUSY 结束后把 coalesce 的 work_fb 局刷上屏（无新 LVGL flush 时补一刀）。 */
static void CommitCoalescedFrame(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->coalesce_pending || ctx->freeze_updates || ctx->defer_standby_paint ||
        ctx->absorb_flush || ctx->gray_hold) {
        return;
    }
    if (!TryFinishInflightRefresh(ctx)) {
        ArmCoalesceKick(ctx);
        return;
    }
    if (!ctx->has_last || ctx->work_fb == nullptr || ctx->last_fb == nullptr || ctx->panel == nullptr) {
        ctx->coalesce_pending = false;
        return;
    }
    if (ctx->force_next_full) {
        // 「立即全刷」碰上屏幕正忙（比如快捷面板刚收起）：别在这里补局刷把它拖到下一次翻页，
        // 让 LVGL 再出一帧，走正常 flush 里的全刷分支
        if (lv_obj_t* scr = lv_screen_active()) lv_obj_invalidate(scr);
        return;
    }

    int cx = 0, cy = 0, cw = ctx->panel_w, ch = ctx->panel_h;
    ExpandPartialDirty(ctx, &cx, &cy, &cw, &ch);
    if (cw <= 0 || ch <= 0) {
        ctx->coalesce_pending = false;
        return;
    }
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    const esp_err_t err =
        DrawPartialDirty(ctx, ctx->panel, ctx->last_fb, ctx->work_fb, 0, 0, ctx->panel_w, ctx->panel_h);
    if (err != ESP_OK) {
        ctx->suppress_lvgl_notify = false;
        ctx->coalesce_pending = false;
        ESP_LOGW(TAG, "coalesce commit: %s", esp_err_to_name(err));
        return;
    }

    ctx->refresh_inflight = true;
    AccountPartial(ctx, cw, ch);
    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    ctx->coalesce_pending = false;
    ESP_LOGD(TAG, "coalesce commit after busy");
}

static void CoalesceKickAsync(void* user_data) {
    auto* ctx = static_cast<EpdFlushCtx*>(user_data);
    if (ctx == nullptr) {
        return;
    }
    ctx->coalesce_kick_armed = false;
    // 已在 LVGL 线程（lv_async）；勿再 DisplayLock，避免与 adapter 锁重入死锁
    CommitCoalescedFrame(ctx);
}

/** 局刷后面板保持上电（省每次上下电约 220ms）；空闲 2.5 秒再掉电，省电也不让面板长时间带高压 */
static unsigned s_vcom_parks = 0;  // 调试：拉回 VCOM 的次数（DebugInfo 里看）
static void PanelIdleOffCb(lv_timer_t* t) {
    auto* ctx = static_cast<EpdFlushCtx*>(lv_timer_get_user_data(t));
    if (ctx == nullptr || ctx->panel == nullptr || !epaper_panel_analog_on(ctx->panel)) return;
    // 看图出灰期间不掉电：灰度那次用的 VCOM 不是 0，掉电那一下会把灰阶往白/灰推，整屏发脏（用户实测约 1 秒后变灰）
    if (ctx->gray_hold) return;
    if (!ctx->refresh_inflight || TryFinishInflightRefresh(ctx)) {
        if (epaper_panel_park_vcom(ctx->panel) == ESP_OK) ++s_vcom_parks;  // 刷完就把 VCOM 拉回 0
    }
    if (esp_timer_get_time() - ctx->last_partial_us < 2500000) return;
    if (ctx->coalesce_pending) return;
    if (ctx->refresh_inflight && !TryFinishInflightRefresh(ctx)) return;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = epaper_panel_power_off_if_on(ctx->panel);
    ESP_LOGI(TAG, "epd idle power off %s (%dms)", esp_err_to_name(err), static_cast<int>((esp_timer_get_time() - t0) / 1000));
}

static void CoalesceEspTimerCb(void* arg) {
    // 勿在 timer 任务里直接碰 panel/LVGL：投递到 LVGL 线程
    if (lv_async_call(CoalesceKickAsync, arg) != LV_RESULT_OK) {
        auto* ctx = static_cast<EpdFlushCtx*>(arg);
        if (ctx != nullptr) {
            ctx->coalesce_kick_armed = false;
        }
    }
}

static void ArmCoalesceKick(EpdFlushCtx* ctx) {
    if (ctx == nullptr || ctx->coalesce_timer == nullptr || ctx->freeze_updates ||
        ctx->defer_standby_paint || ctx->absorb_flush || ctx->gray_hold) {
        return;
    }
    if (!ctx->coalesce_pending) {
        return;
    }
    // 已在跑则等回调；回调里若仍 BUSY 会再 arm
    if (esp_timer_is_active(ctx->coalesce_timer)) {
        ctx->coalesce_kick_armed = true;
        return;
    }
    ctx->coalesce_kick_armed = true;
    if (esp_timer_start_once(ctx->coalesce_timer, 50000) != ESP_OK) { // 50ms
        ctx->coalesce_kick_armed = false;
    }
}

/** A2I1 逻辑竖屏 → 面板 HTILED（与 adapter ROTATE_270：px=ly, py=ph-1-lx）。 */
void BlitA2i1ToPanelFb(uint8_t* dst_fb, int panel_w, int panel_h, const uint8_t* a2i1_payload,
                       uint16_t log_w, uint16_t log_h, uint16_t src_stride) {
    const uint8_t* src = a2i1_payload + 8; /* skip LVGL palette */
    const int dst_stride = panel_w / 8;
    memset(dst_fb, kLvglWhite, static_cast<size_t>(dst_stride) * static_cast<size_t>(panel_h));
    for (int ly = 0; ly < static_cast<int>(log_h); ly++) {
        for (int lx = 0; lx < static_cast<int>(log_w); lx++) {
            const bool white =
                (src[ly * src_stride + (lx >> 3)] & static_cast<uint8_t>(0x80 >> (lx & 7))) != 0;
            const int px = ly;
            const int py = panel_h - 1 - lx;
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst_fb[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
    }
}

/** 关机提示条：叠在关机图顶部（逻辑坐标 x=16,y=16），坐标映射同 BlitA2i1ToPanelFb。 */
void OverlayShutdownHint(uint8_t* dst_fb, int panel_w, int panel_h) {
    constexpr int kX0 = 16, kY0 = 16;
    const int dst_stride = panel_w / 8;
    const int src_stride = kShutdownHintW / 8;
    for (int y = 0; y < kShutdownHintH; y++) {
        for (int x = 0; x < kShutdownHintW; x++) {
            const int lx = kX0 + x, ly = kY0 + y;
            const int px = ly, py = panel_h - 1 - lx;
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) continue;
            const bool white = (kShutdownHintBits[y * src_stride + (x >> 3)] & (0x80 >> (x & 7))) != 0;
            uint8_t* byte = &dst_fb[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) *byte |= mask; else *byte = static_cast<uint8_t>(*byte & ~mask);
        }
    }
}

/** 关机：关机图一次全刷（0xC7，与进/出待机相同）。 */
esp_err_t EpdShutdownFullFb(EpdFlushCtx* ctx, const uint8_t* curr_fb) {
    if (ctx == nullptr || ctx->panel == nullptr || ctx->last_fb == nullptr || curr_fb == nullptr ||
        ctx->work_fb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    DrainRefreshIfNeeded(ctx);
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);

    ESP_LOGD(TAG, "shutdown: FULL 0xC7");
    esp_err_t err = FullRefreshBothSame(ctx, curr_fb, "shutdown", SSD1677_EPAPER_REFRESH_STANDBY);
    ESP_LOGD(TAG, "shutdown: done err=%s", esp_err_to_name(err));
    ctx->suppress_lvgl_notify = false;
    if (err == ESP_OK) {
        memcpy(ctx->last_fb, curr_fb, ctx->fb_size);
        ctx->has_last = true;
    }
    return err;
}

/**
 * 看图保灰用的局刷波形：在 kEpdLutPartial 基础上，黑→黑那行不再加 VSH2 压黑（全 VSS），
 * VCOM 用 0x30（和出灰那次一致；普通局刷是 0x00，会给所有像素一个推白的直流场）。只有真正变化的像素被驱动。
 */
static const uint8_t kEpdLutPartialHold[112] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 行0 黑→黑：VSS
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 行1 黑→白：VSL
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 行2 白→黑：VSH1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 行3 白→白：VSS
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 行4 VCOM=DC
    0x0D, 0x00, 0x00, 0x00, 0x00,                                // 组0 TPA=13 帧
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 组1..9 全 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22,
    0x17, 0x41, 0xA8, 0x32, 0x30,
    0x00, 0x00,
};
static volatile bool s_gray_hold_lut = true;  // false = 退回「状态栏攒着不刷」
static bool GrayHoldLutOn() { return s_gray_hold_lut; }

// GCC 在超大翻译单元内于 EpdLvglDrawBitmap 中展开 ESP_LOGI 可能 ICE，日志单独成函数
void LogAaOffDualPlanePartialSeed() {
    ESP_LOGD(TAG, "aa_off dual-plane PARTIAL 0x26=old 0x24=new");
}

esp_err_t EpdLvglDrawBitmap(lv_display_t* disp, esp_lcd_panel_handle_t panel, int x_start,
                            int y_start, int x_end, int y_end, const void* color_map,
                            void* user_ctx) {
    (void)disp;
    auto* ctx = static_cast<EpdFlushCtx*>(user_ctx);
    if (!ctx || !color_map || !ctx->last_fb || !ctx->work_fb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->freeze_updates) {
        if (ctx->disp) {
            lv_display_flush_ready(ctx->disp);
        }
        return ESP_OK;
    }
    if (ctx->gray_hold && !ctx->absorb_flush) {
        // 先把这次重画合进 work_fb，再看状态栏以外有没有像素变化：没有就继续攒着不上屏
        // （有时 LVGL 会整屏重画，但画出来的东西和屏上一样）
        int ax = 0, ay = 0, aw = 0, ah = 0;
        AlignArea(x_start, y_start, x_end, y_end, &ax, &ay, &aw, &ah);
        if (aw > 0 && ah > 0) {
            if (!ctx->coalesce_pending) memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
            OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, ax, ay,
                         aw, ah);
            ctx->coalesce_pending = true;
        }
        // 竖屏逻辑 y = 面板 x：只比较面板每行里 x >= hold_h 的字节
        const int stride = ctx->panel_w / 8;
        const int b0 = (ctx->gray_hold_h + 7) / 8;
        bool changed = false;
        for (int py = 0; py < ctx->panel_h && !changed; ++py) {
            changed = memcmp(ctx->work_fb + py * stride + b0, ctx->last_fb + py * stride + b0, stride - b0) != 0;
        }
        if (!changed) {
            if (GrayHoldLutOn() && ctx->coalesce_pending && memcmp(ctx->work_fb, ctx->last_fb, ctx->fb_size) != 0) {
                // 状态栏照常刷，但用「保持型」局刷波形：没变化的像素一律不驱动、VCOM 和出灰时一致，灰就不会漂
                DrainRefreshIfNeeded(ctx);
                ctx->suppress_lvgl_notify = true;
                xSemaphoreTake(ctx->done_sem, 0);
                esp_err_t err = epaper_panel_load_custom_lut(ctx->panel, kEpdLutPartialHold);
                if (err == ESP_OK) {
                    epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
                    err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->work_fb);
                }
                if (err == ESP_OK) {
                    epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_CUSTOM);
                    err = epaper_panel_refresh_screen(ctx->panel);
                }
                WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, "gray_hold_partial");
                xSemaphoreTake(ctx->done_sem, 0);
                ctx->suppress_lvgl_notify = false;
                memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
                ctx->coalesce_pending = false;
                ESP_LOGI(TAG, "gray hold partial (hold lut) busy=%ums -> %s",
                         (unsigned)epaper_panel_last_busy_ms(ctx->panel), esp_err_to_name(err));
            }
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
        ctx->gray_hold = false;  // 有正经内容要画（换图、拉面板）：照常刷
        ESP_LOGI(TAG, "gray hold off by area (%d,%d)-(%d,%d)", x_start, y_start, x_end, y_end);
    }
    if (ctx->absorb_flush) {
        int ax = 0, ay = 0, aw = 0, ah = 0;
        AlignArea(x_start, y_start, x_end, y_end, &ax, &ay, &aw, &ah);
        if (aw > 0 && ah > 0) {
            if (!ctx->coalesce_pending) memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
            OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, ctx->panel_h, ax, ay,
                         aw, ah);
            ctx->coalesce_pending = true;  // 先攒着；ApplyGray4Single 拿 work_fb 当新底
        }
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    int x_draw = 0, y_draw = 0, w_draw = 0, h_draw = 0;
    AlignArea(x_start, y_start, x_end, y_end, &x_draw, &y_draw, &w_draw, &h_draw);
    const int log_w = DisplayUiLogW();
    const int log_h = DisplayUiLogH();
    if (w_draw <= 0 || h_draw <= 0 || x_draw < 0 || y_draw < 0 ||
        x_draw + w_draw > log_w || y_draw + h_draw > log_h) {
        ESP_LOGW(TAG, "bad area (%d,%d)-(%d,%d) log=%dx%d", x_start, y_start, x_end, y_end, log_w,
                 log_h);
        return ESP_ERR_INVALID_ARG;
    }

    // 勿在 flush_cb 里 Wait BUSY：否则 LVGL 跑不了 indev，touch_feed 边沿无法交付
    if (!TryFinishInflightRefresh(ctx)) {
        if (!ctx->coalesce_pending) {
            if (ctx->boot_fb_ready || ctx->has_last) {
                memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
            } else {
                memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
            }
        }
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w,
                     ctx->panel_h, x_draw, y_draw, w_draw, h_draw);
        if (ctx->defer_standby_paint) {
            memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
            ctx->coalesce_pending = false;
            ctx->coalesce_kick_armed = false;
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
        ctx->coalesce_pending = true;
        ArmCoalesceKick(ctx);
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (ctx->coalesce_pending) {
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w,
                     ctx->panel_h, x_draw, y_draw, w_draw, h_draw);
    } else if (ctx->boot_fb_ready || ctx->has_last) {
        memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w,
                     ctx->panel_h, x_draw, y_draw, w_draw, h_draw);
    } else {
        memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w,
                     ctx->panel_h, x_draw, y_draw, w_draw, h_draw);
    }

    if (ctx->defer_boot_paint) {
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->boot_fb_ready = true;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (ctx->defer_standby_paint) {
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (!ctx->has_last) {
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        esp_err_t err = ESP_OK;
        if (WantReaderGrayAa()) {
            err = GrayRefreshFromWorkFb(ctx, "first_paint_gray", true);
        } else {
            err = FullRefreshBothSame(ctx, ctx->work_fb, "first_paint");
        }
        ctx->suppress_lvgl_notify = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "first paint: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->has_last = true;
        ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // 阅读抗锯齿：局刷换 BW 底 + Overlay 灰；仅强制/周期才 C4 底
    if (WantReaderGrayAa()) {
        // AA 开瞬间：优先双平面局刷底，勿被 force_next_full / 周期 C4 抢走
        const bool dual_seed = ctx->force_dual_plane_partial;
        ctx->force_dual_plane_partial = false;
        const bool force_full_base =
            !dual_seed &&
            (ctx->force_next_full || ctx->partial_refresh_count >= PartialsPerFull());
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        DrainRefreshIfNeeded(ctx);
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        esp_err_t err = GrayRefreshFromWorkFb(ctx, dual_seed ? "aa_on_gray" : "reader_gray",
                                             force_full_base);
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reader gray: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // AA 关瞬间：双平面局刷 seed 0x26/0x24（无灰 Overlay），再回日常只写 0x24
    if (ctx->force_dual_plane_partial) {
        ctx->force_dual_plane_partial = false;
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        DrainRefreshIfNeeded(ctx);
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        LogAaOffDualPlanePartialSeed();
        esp_err_t err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, "aa_off_seed");
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "aa_off seed: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->partial_refresh_count++;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // 退待机后首帧 / 周期或其它强制全刷（FULL 与 STANDBY 均为 0xC7）
    if (ctx->force_next_full || ctx->partial_refresh_count >= PartialsPerFull()) {
        const bool from_force = ctx->force_next_full;
        const bool use_c7 = from_force && ctx->force_full_c7;
        ctx->force_next_full = false;
        ctx->force_full_c7 = false;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        const auto mode = use_c7 ? SSD1677_EPAPER_REFRESH_STANDBY : SSD1677_EPAPER_REFRESH_FULL;
        ESP_LOGD(TAG, "%s FULL 0x22=%02X", from_force ? "forced" : "periodic",
                 use_c7 ? SSD1677_PARAM_DISP_UPDATE_FULL_STANDBY : SSD1677_PARAM_DISP_UPDATE_FULL);
        const int64_t t_full = esp_timer_get_time();
        esp_err_t err = FullRefreshBothSame(ctx, ctx->work_fb,
                                            from_force ? "forced_full" : "periodic_full", mode);
        ESP_LOGI(TAG, "epd %s full: busy=%ums total=%dms", from_force ? "forced" : "periodic",
                 (unsigned)epaper_panel_last_busy_ms(ctx->panel),
                 (int)((esp_timer_get_time() - t_full) / 1000));
        ctx->suppress_lvgl_notify = false;
        ctx->refresh_inflight = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "full: %s", esp_err_to_name(err));
            return err;
        }
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    const bool was_coalesce = ctx->coalesce_pending;
    if (was_coalesce) {
        // 合并帧：发送仍走整帧；面积只用于全刷计数
        x_draw = 0;
        y_draw = 0;
        w_draw = ctx->panel_w;
        h_draw = ctx->panel_h;
        ExpandPartialDirty(ctx, &x_draw, &y_draw, &w_draw, &h_draw);
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        if (w_draw <= 0 || h_draw <= 0) {
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
    } else {
        // OverlayDirty 已写入面板 FB；脏矩形是逻辑坐标，XOR 须扫物理整帧
        x_draw = 0;
        y_draw = 0;
        w_draw = ctx->panel_w;
        h_draw = ctx->panel_h;
        ExpandPartialDirty(ctx, &x_draw, &y_draw, &w_draw, &h_draw);
        if (w_draw <= 0 || h_draw <= 0) {
            lv_display_flush_ready(ctx->disp);
            return ESP_OK;
        }
    }

    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);

    // 局刷：整帧 0x24=新（不写 0x26）
    ESP_LOGD(TAG, "PARTIAL full-frame 0x24 #%u", ctx->partial_refresh_count + 1);
    {
        const esp_err_t err =
            DrawPartialDirty(ctx, panel, ctx->last_fb, ctx->work_fb, x_draw, y_draw, w_draw, h_draw);
        if (err != ESP_OK) {
            ctx->suppress_lvgl_notify = false;
            ESP_LOGE(TAG, "partial: %s", esp_err_to_name(err));
            return err;
        }
    }

    ctx->refresh_inflight = true;
    AccountPartial(ctx, w_draw, h_draw);

    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    lv_display_flush_ready(ctx->disp);
    return ESP_OK;
}

/** SetupUI 末尾：首页 FULL（0x24=0x26）；之后局刷只写整帧 0x24。 */
void EpdCommitBootBaseline(EpdFlushCtx* ctx)
{
    if (ctx == nullptr || !ctx->defer_boot_paint) {
        return;
    }
    ctx->defer_boot_paint = false;
    if (!ctx->boot_fb_ready || ctx->last_fb == nullptr || ctx->panel == nullptr) {
        ESP_LOGW(TAG, "boot paint skip (no fb)");
        return;
    }
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    esp_err_t err = FullRefreshBothSame(ctx, ctx->last_fb, "boot");
    if (err == ESP_OK) {
        ctx->has_last = true;
        ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
    } else {
        ESP_LOGE(TAG, "boot paint: %s", esp_err_to_name(err));
    }
    ctx->suppress_lvgl_notify = false;
}

}  // namespace

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    instance_ = this;
    {
        Settings st(kEpdNvsNs, false);
        s_partials_per_full.store(st.GetInt(kEpdNvsFullEvery, kDefaultPartialsPerFull));
        ESP_LOGI(TAG, "nvs load full_every=%d", s_partials_per_full.load());
    }
    width_ = width;
    height_ = height;
    if (touch_handle != nullptr) {
        s_touch_tp = touch_handle;
    }

    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                auto* display = static_cast<LVAdapterDisplay*>(arg);
                DisplayLockGuard lock(display);
                if (display->notification_label_) {
                    lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                }
                if (display->status_label_ && !AssistantScreen::IsPttWaveVisible()) {
                    lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
                }
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "epd_notif_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    epd_flush_ctx_ = new EpdFlushCtx();
    epd_flush_ctx_->panel = panel;
    epd_flush_ctx_->panel_w = SSD1677_PANEL_WIDTH;
    epd_flush_ctx_->panel_h = SSD1677_PANEL_HEIGHT;
    epd_flush_ctx_->fb_size = SSD1677_PANEL_BUFFER_SIZE;
    epd_flush_ctx_->done_sem = xSemaphoreCreateBinary();

    {
        esp_timer_create_args_t coalesce_args = {
            .callback = CoalesceEspTimerCb,
            .arg = epd_flush_ctx_,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "epd_coalesce",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&coalesce_args, &epd_flush_ctx_->coalesce_timer));
    }
    // last/work 在 PSRAM（仅 CPU 读写）。发屏由面板侧拷到内部 DMA bounce，
    // 避免 PSRAM 直 DMA 的 cache 不一致与鬼影。内部只常驻 1×48KB bounce。
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    epd_flush_ctx_->last_fb =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->work_fb =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->gray_scratch =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));

    if (epd_flush_ctx_->last_fb) {
        memset(epd_flush_ctx_->last_fb, kLvglWhite, epd_flush_ctx_->fb_size);
    }

    if (!epd_flush_ctx_->done_sem || !epd_flush_ctx_->last_fb || !epd_flush_ctx_->work_fb ||
        !epd_flush_ctx_->gray_scratch) {
        ESP_LOGE(TAG, "epd flush ctx alloc failed");
    }
    epd_gray_aa_ensure(SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 16 * 1024;
    adapter_cfg.task_max_delay_ms = 1000;
    // PSRAM 栈上禁止调用 spi_flash/NVS（会触发 cache_utils assert）。
    // 业务里凡 EnsureAudioServiceRunning / Settings 等须放到 DRAM 栈任务（参见录音/翻译）。
    adapter_cfg.stack_in_psram = true;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));
    ESP_ERROR_CHECK(esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false));

    // ROTATE_0：adapter 原样交出 I1；逻辑→面板旋转由 DisplayUi / OverlayDirty 做
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_MONO_DEFAULT_CONFIG(
        panel, panel_io, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        ESP_LV_ADAPTER_ROTATE_0, ESP_LV_ADAPTER_MONO_LAYOUT_HTILED);
    disp_cfg.profile.use_psram = true;

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_ERROR_CHECK(disp ? ESP_OK : ESP_FAIL);
    epd_flush_ctx_->disp = disp;
    s_epd_panel = panel;
    DisplayUiBind(disp, SSD1677_PANEL_WIDTH, SSD1677_PANEL_HEIGHT);
    DisplayUiSetOrient(kDisplayUiPortrait);
    lv_display_add_event_cb(disp, OnLvglRefrStart, LV_EVENT_REFR_START, nullptr);

    epaper_panel_callbacks_t epd_cbs = {
        .on_epaper_refresh_done = EpdRefreshDoneCb,
    };
    ESP_ERROR_CHECK(epaper_panel_register_event_callbacks(panel, &epd_cbs, epd_flush_ctx_));
    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(disp, EpdAreaRounder, nullptr));

    esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = {
        .custom_draw_bitmap = EpdLvglDrawBitmap,
    };
    ESP_ERROR_CHECK(
        esp_lv_adapter_set_draw_bitmap_callbacks(disp, &draw_cbs, epd_flush_ctx_));

    if (touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        touch_cfg.callbacks.custom_touch_read = CustomTouchRead;
        touch_indev_ = esp_lv_adapter_register_touch(&touch_cfg);
        ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_FAIL);
        s_press_disp = disp;
        s_press_gray_hold = &epd_flush_ctx_->gray_hold;
        lv_indev_add_event_cb(touch_indev_, OnPressIndevEvent, LV_EVENT_PRESSED, nullptr);
        lv_indev_add_event_cb(touch_indev_, OnPressIndevEvent, LV_EVENT_RELEASED, nullptr);
        StartTouchFeed(touch_handle);
    }

    lv_timer_create(PanelIdleOffCb, 100, epd_flush_ctx_);  // 100ms：刷完尽快拉回 VCOM
    ESP_ERROR_CHECK(esp_lv_adapter_start());

    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &resources_assets_));

    esp_lv_fs_handle_t fs_handle = nullptr;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = resources_assets_,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    // 在内部 RAM 栈的初始化线程预读 NVS，避免首次进首页/设置时在 LVGL(PSRAM 栈)里碰 flash。
    (void)HomeScreen::LoadCardStyle();
    (void)HapticIsEnabled();
    BookReaderPrefsEnsureLoaded();
    BookTapZonesEnsureLoaded();
    // 待机壁纸路由：开机即灌 NVS→缓存，勿等 Application::Start 联网后才 classic
    Wallpaper_HydrateFromNvsNow();
    // 阅读首页快照：与壁纸同点灌入，首页可点时内存已有最近阅读/总时长
    reader::book_home_snapshot::HydrateFromNvs();

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }
}

void LVAdapterDisplay::SetupUI() {
    if (touch_indev_ == nullptr) {
        ESP_LOGW(TAG, "SetupUI: no touch, load TouchMissingScreen");
        lv_screen_load(TouchMissingScreen::Create());
    } else {
        ESP_LOGD(TAG, "SetupUI: create HomeScreen");
        lv_screen_load(HomeScreen::Create());
        ESP_LOGD(TAG, "SetupUI: HomeScreen loaded");
    }
    // 攒帧后黑→白→首页上屏
    if (lv_screen_active() != nullptr) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
    }
    EpdCommitBootBaseline(epd_flush_ctx_);
}

void LVAdapterDisplay::KickTouchInput() {
    if (touch_indev_ != nullptr) {
        esp_lv_adapter_touch_notify_interrupt(touch_indev_);
    }
}

bool LVAdapterDisplay::TryGetResource(const char* name, const uint8_t** mem, size_t* size) const {
    if (name == nullptr || name[0] == '\0' || mem == nullptr || size == nullptr) {
        return false;
    }
    *mem = nullptr;
    *size = 0;
    if (resources_assets_ == nullptr) {
        return false;
    }
    const int n = mmap_assets_get_stored_files(resources_assets_);
    for (int i = 0; i < n; ++i) {
        const char* nm = mmap_assets_get_name(resources_assets_, i);
        if (nm == nullptr || std::strcmp(nm, name) != 0) {
            continue;
        }
        const uint8_t* p = mmap_assets_get_mem(resources_assets_, i);
        const int sz = mmap_assets_get_size(resources_assets_, i);
        if (p == nullptr || sz <= 0) {
            return false;
        }
        *mem = p;
        *size = static_cast<size_t>(sz);
        return true;
    }
    return false;
}

bool TouchUiFingerIsDown(void) {
    portENTER_CRITICAL(&s_touch_snap_mux);
    // live 仍按着，且没有未交付的抬起（抬起已锁存说明手指已离开）
    const bool down = s_touch_snap.live_count > 0 && s_touch_snap.pending_up == 0;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    return down;
}

bool TouchUiHasPendingEdges(void) {
    portENTER_CRITICAL(&s_touch_snap_mux);
    const bool pending = s_touch_snap.pending_down > 0 || s_touch_snap.pending_up > 0;
    portEXIT_CRITICAL(&s_touch_snap_mux);
    return pending;
}

void LVAdapterDisplay::RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count,
                                                TouchVirtualKeyEventCb cb, void* user_data) {
    s_vk_count = 0;
    s_vk_cb = cb;
    s_vk_user = user_data;
    s_vk_instant_was_down = false;
    s_vk_hold = {};
    if (keys == nullptr || count == 0) {
        return;
    }
    if (count > kMaxTouchVirtualKeys) {
        count = kMaxTouchVirtualKeys;
    }
    for (size_t i = 0; i < count; ++i) {
        s_vk_keys[i] = keys[i];
    }
    s_vk_count = count;
    ESP_LOGD(TAG, "registered %u touch virtual keys", static_cast<unsigned>(s_vk_count));
}

void LVAdapterDisplay::RestoreStatusWidgetsLocked() {
    // 切页只换控件指针，图标/静音/时钟缓存跨页保留，首帧与页面内容同一次局刷画出。
    // 局刷已改为启动后立刻 flush_ready（BUSY 延后到下次 Drain），状态栏合并刷新仍有助于少刷。
    if (network_label_ != nullptr) {
        lv_label_set_text(network_label_, network_icon_ != nullptr ? network_icon_ : "");
    }
    if (battery_label_ != nullptr) {
        lv_label_set_text(battery_label_, battery_icon_ != nullptr ? battery_icon_ : "");
    }
    if (battery_pct_label_ != nullptr) {
        lv_label_set_text(battery_pct_label_, battery_pct_cache_);
    }
    if (mute_label_ != nullptr) {
        lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
    }
    if (status_label_ != nullptr) {
        // 无缓存时勿回退「待命」：首页 Idle 应以时钟为准，由 ApplyIdleStatusBar 写入
        const char* body = status_body_cache_[0] != '\0' ? status_body_cache_ : "--:--";
        ApplyStatusTextLocked(body);
        if (!AssistantScreen::IsPttWaveVisible()) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            if (notification_label_ != nullptr) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LVAdapterDisplay::BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery,
                                         lv_obj_t* status, lv_obj_t* notification,
                                         lv_obj_t* low_battery_popup, lv_obj_t* battery_pct) {
    network_label_ = network;
    mute_label_ = mute;
    battery_label_ = battery;
    battery_pct_label_ = battery_pct;
    status_label_ = status;
    notification_label_ = notification;
    low_battery_popup_ = low_battery_popup;

    // 勿清 battery_icon_/network_icon_/muted_/status_body_cache_，也勿调 Board/UpdateStatusBar
    // （SetupUI 持锁重入会死锁）。有缓存则立刻灌进新控件，与本页首帧合并刷新。
    RestoreStatusWidgetsLocked();

    if (pending_notification_[0] != '\0' && notification_label_ != nullptr) {
        lv_label_set_text(notification_label_, pending_notification_);
        lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (notification_timer_ != nullptr) {
            esp_timer_stop(notification_timer_);
            if (pending_notification_ms_ > 0) {
                esp_timer_start_once(notification_timer_,
                                     static_cast<uint64_t>(pending_notification_ms_) * 1000ULL);
            }
        }
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
    }
}

void LVAdapterDisplay::QueueStatusNotification(const char* notification, int duration_ms) {
    if (notification == nullptr || notification[0] == '\0') {
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
        return;
    }
    std::snprintf(pending_notification_, sizeof(pending_notification_), "%s", notification);
    pending_notification_ms_ = duration_ms;
    // 若状态栏已绑定，直接显示
    if (notification_label_ != nullptr) {
        ShowNotification(pending_notification_, duration_ms);
        pending_notification_[0] = '\0';
        pending_notification_ms_ = 0;
    }
}

LVAdapterDisplay::~LVAdapterDisplay() {
    if (instance_ == this) {
        instance_ = nullptr;
    }
    StopTouchFeed();
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
        notification_timer_ = nullptr;
    }
    if (epd_flush_ctx_) {
        if (epd_flush_ctx_->coalesce_timer) {
            esp_timer_stop(epd_flush_ctx_->coalesce_timer);
            esp_timer_delete(epd_flush_ctx_->coalesce_timer);
            epd_flush_ctx_->coalesce_timer = nullptr;
        }
        if (epd_flush_ctx_->last_fb) {
            heap_caps_free(epd_flush_ctx_->last_fb);
        }
        if (epd_flush_ctx_->work_fb) {
            heap_caps_free(epd_flush_ctx_->work_fb);
        }
        if (epd_flush_ctx_->done_sem) {
            vSemaphoreDelete(epd_flush_ctx_->done_sem);
        }
        delete epd_flush_ctx_;
        epd_flush_ctx_ = nullptr;
    }
}

void LVAdapterDisplay::SetEmotion(const char* emotion) {
    // 非对话页无表情控件，跳过加锁，少打断其它屏局刷。
    if (!AssistantScreen::IsActive()) {
        return;
    }
    DisplayLockGuard lock(this);
    AssistantScreen::SetEmotion(emotion);
}

void LVAdapterDisplay::SetChatMessage(const char* role, const char* content) {
    // 只转发；AddMessage 内自行短持锁做 UI。解析/落盘在锁外，避免流式每包堵住翻页。
    if (!AssistantScreen::IsActive()) {
        return;
    }
    AssistantScreen::AddMessage(role, content);
}

void LVAdapterDisplay::SetStatusTitlePrefix(const char* prefix) {
    DisplayLockGuard lock(this);
    if (prefix == nullptr || prefix[0] == '\0') {
        status_title_prefix_[0] = '\0';
    } else {
        std::snprintf(status_title_prefix_, sizeof(status_title_prefix_), "%s", prefix);
    }
}

void LVAdapterDisplay::BeginStandbyEnterPaint() {
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr) {
        return;
    }
    DrainRefreshIfNeeded(ctx);
    ctx->defer_standby_paint = true;
    ctx->coalesce_pending = false;
    ctx->coalesce_kick_armed = false;
    ESP_LOGD(TAG, "standby enter: defer paint until FULL");
}

int LVAdapterDisplay::GetFullRefreshEvery() { return s_partials_per_full.load(); }

void LVAdapterDisplay::SetFullRefreshEvery(int n) {
    if (n < 0) n = 0;
    if (n > 200) n = 200;
    if (s_partials_per_full.exchange(n) == n) return;
    ESP_LOGI(TAG, "full refresh every %d partials%s", n, n == 0 ? " (manual only)" : "");
    // NVS 写会关 Flash cache：放到内部 RAM 栈的短任务里做，与 haptic 一致
    xTaskCreate(
        [](void*) {
            Settings st(kEpdNvsNs, true);
            st.SetInt(kEpdNvsFullEvery, s_partials_per_full.load());
            vTaskDelete(nullptr);
        },
        "epd_nvs", 3072, nullptr, 5, nullptr);
}

void LVAdapterDisplay::RequestFullRefreshNow() {
    // 须在 LVGL 线程调用：下一帧走全刷，并整屏失效触发一帧
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr) return;
    ctx->force_next_full = true;
    if (lv_obj_t* scr = lv_screen_active()) {
        lv_obj_invalidate(scr);
    }
}

bool LVAdapterDisplay::CopyPanelFrame(std::vector<uint8_t>& out, int* w, int* h) {
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->fb_size == 0) {
        return false;
    }
    const uint8_t* fb = ctx->has_last ? ctx->last_fb : nullptr;
    if (ctx->coalesce_pending && ctx->work_fb != nullptr) {
        fb = ctx->work_fb;
    }
    if (fb == nullptr) {
        return false;
    }
    out.assign(fb, fb + ctx->fb_size);
    *w = ctx->panel_w;
    *h = ctx->panel_h;
    return true;
}

void LVAdapterDisplay::ParkEpdForStandby() {
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr || ctx->standby_frozen) {
        return;
    }

    DrainRefreshIfNeeded(ctx);
    ctx->defer_standby_paint = false;

    // settle 攒帧完毕：全刷落待机画面后再冻结
    const uint8_t* fb = ctx->last_fb;
    if (ctx->coalesce_pending && ctx->work_fb != nullptr) {
        fb = ctx->work_fb;
    }
    if (fb != nullptr && ctx->has_last && ctx->last_fb != nullptr) {
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        ESP_LOGD(TAG, "standby enter: FULL 0xC7 (content before freeze)");
        const esp_err_t err =
            FullRefreshBothSame(ctx, fb, "standby_enter", SSD1677_EPAPER_REFRESH_STANDBY);
        ctx->suppress_lvgl_notify = false;
        if (err == ESP_OK) {
            if (fb != ctx->last_fb) {
                memcpy(ctx->last_fb, fb, ctx->fb_size);
            }
            ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
            ctx->force_next_full = false;
            ctx->force_full_c7 = false;
        } else {
            ESP_LOGW(TAG, "standby enter FULL: %s", esp_err_to_name(err));
        }
    }
    ctx->coalesce_pending = false;
    ctx->coalesce_kick_armed = false;
    ctx->refresh_inflight = false;

    // 冻结后续 LVGL flush；不做 0x10 Deep Sleep
    ctx->freeze_updates = true;
    ctx->standby_frozen = true;
    ESP_LOGD(TAG, "standby: flush frozen (no deep sleep)");
}

void LVAdapterDisplay::PauseStandbyBackgroundWork() {
    if (s_standby_background_paused) {
        return;
    }
    s_standby_background_paused = true;
    const bool had_touch_feed = (s_touch_feed_task != nullptr);
    if (had_touch_feed) {
        StopTouchFeed();
    }
    // 勿 lv_timer_enable(false)：lv_async_call 也是 lv_timer， dismiss 会永远排不上
    ESP_LOGD(TAG, "standby: paused touch_feed(%d)", had_touch_feed ? 1 : 0);
}

void LVAdapterDisplay::ResumeStandbyBackgroundWork() {
    if (!s_standby_background_paused) {
        return;
    }
    s_standby_background_paused = false;
    if (s_touch_tp != nullptr && s_touch_feed_task == nullptr) {
        StartTouchFeed(s_touch_tp);
    }
    ESP_LOGD(TAG, "standby: resumed touch_feed");
}

void LVAdapterDisplay::WakeEpdFromStandby() {
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr) {
        return;
    }
    const bool was_defer = ctx->defer_standby_paint;
    const bool was_frozen = ctx->standby_frozen;
    ctx->defer_standby_paint = false;
    ctx->coalesce_pending = false;
    ctx->coalesce_kick_armed = false;
    if (!was_defer && !was_frozen) {
        return;
    }
    if (was_frozen) {
        ctx->standby_frozen = false;
        ctx->freeze_updates = false;
    }
    // settle 前退出或正常 dismiss：等卸 Overlay 后首帧全刷底层（0xC7）
    ctx->force_next_full = true;
    ctx->force_full_c7 = true;
    ESP_LOGD(TAG, "standby: exit defer/freeze (next frame FULL 0xC7)");
}

namespace {
/** 「11月3日  星期二  农历九月廿五」：本地算，不靠服务器那张卡（断网时卡上的日期会过期） */
std::string LocalDateLine() {
    const time_t now = time(nullptr);
    struct tm t {};
    if (now < 1700000000 || localtime_r(&now, &t) == nullptr) return "";
    static const char* kWeek[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d月%d日  星期%s", t.tm_mon + 1, t.tm_mday, kWeek[t.tm_wday]);
    std::string line = buf;
    // 农历：查表（2025~2040）
    struct tm base {};
    base.tm_year = kLunarY0 - 1900;
    base.tm_mon = 0;
    base.tm_mday = 1;
    base.tm_hour = 12;
    struct tm today = t;
    today.tm_hour = 12;
    today.tm_min = today.tm_sec = 0;
    const long di = static_cast<long>((mktime(&today) - mktime(&base)) / 86400);
    if (di >= 0 && di < kLunarDays) {
        static const char* kNum[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
        static const char* kMon[] = {"", "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
        const int m = kLunar[di * 2] & 0x7F, leap = kLunar[di * 2] & 0x80, d = kLunar[di * 2 + 1];
        std::string ds;
        if (d == 10) ds = "初十";
        else if (d == 20) ds = "二十";
        else if (d == 30) ds = "三十";
        else ds = std::string(d < 10 ? "初" : (d < 20 ? "十" : (d < 30 ? "廿" : "卅"))) + kNum[d % 10];
        if (m >= 1 && m <= 12) line += std::string("  农历") + (leap ? "闰" : "") + kMon[m] + "月" + ds;
    }
    return line;
}

const DateGlyph* FindDateGlyph(const char* p, int* len) {
    for (const auto& g : kDateGlyphs) {
        const int n = static_cast<int>(std::strlen(g.utf8));
        if (std::strncmp(p, g.utf8, n) == 0) {
            *len = n;
            return &g;
        }
    }
    *len = 1;
    return nullptr;
}
}  // namespace

int LVAdapterDisplay::StandbyDrawClock(int hh, int mm, int* err_out) {
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr || !ctx->standby_frozen || ctx->last_fb == nullptr ||
        ctx->work_fb == nullptr) {
        return 1;
    }
    // 逻辑坐标（竖屏 480×800）：顶部 0..120 是信息卡留给时钟的空白区
    constexpr int kTop = 8, kBottom = 112;
    const int idx[5] = {hh / 10 % 10, hh % 10, 10, mm / 10 % 10, mm % 10};
    constexpr int kGap = 6;
    int total = 0;
    for (int i = 0; i < 5; ++i) total += kClockGlyphs[idx[i]].w + (i ? kGap : 0);
    const int log_w = ctx->panel_h;  // 竖屏：逻辑宽 = 面板高
    int x = (log_w - total) / 2;
    const int y0 = (120 - kClockGlyphH) / 2;
    const int pw = ctx->panel_w, ph = ctx->panel_h, stride = pw / 8;

    memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
    auto set_px = [&](int lx, int ly, bool black) {
        const int px = ly, py = ph - 1 - lx;
        if (px < 0 || py < 0 || px >= pw || py >= ph) return;
        uint8_t* byte = &ctx->work_fb[py * stride + (px >> 3)];
        const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
        if (black) *byte = static_cast<uint8_t>(*byte & ~mask); else *byte |= mask;
    };
    for (int ly = kTop; ly < kBottom; ++ly)
        for (int lx = 0; lx < log_w; ++lx) set_px(lx, ly, false);
    for (int i = 0; i < 5; ++i) {
        const ClockGlyph& g = kClockGlyphs[idx[i]];
        for (int gy = 0; gy < kClockGlyphH; ++gy)
            for (int gx = 0; gx < g.w; ++gx)
                if (g.bits[gy * g.stride + (gx >> 3)] & (0x80 >> (gx & 7))) set_px(x + gx, y0 + gy, true);
        x += g.w + kGap;
    }
    // 日期行：盖掉卡片上服务器画的那行（逻辑 y 120..162，下面 164 是分隔线）
    {
        const std::string line = LocalDateLine();
        if (!line.empty()) {
            constexpr int kDateTop = 118, kDateBottom = 162;
            int tw = 0;
            for (const char* q = line.c_str(); *q;) {
                int n = 1;
                const DateGlyph* g = FindDateGlyph(q, &n);
                tw += g ? g->w : 0;
                q += n;
            }
            for (int ly = kDateTop; ly < kDateBottom; ++ly)
                for (int lx = 0; lx < log_w; ++lx) set_px(lx, ly, false);
            int dx = (log_w - tw) / 2;
            const int dy = 120 + (40 - kDateGlyphH) / 2;
            for (const char* q = line.c_str(); *q;) {
                int n = 1;
                const DateGlyph* g = FindDateGlyph(q, &n);
                q += n;
                if (!g) continue;
                for (int gy = 0; gy < kDateGlyphH; ++gy)
                    for (int gx = 0; gx < g->w; ++gx)
                        if (g->bits[gy * g->stride + (gx >> 3)] & (0x80 >> (gx & 7))) set_px(dx + gx, dy + gy, true);
                dx += g->w;
            }
        }
    }
    if (memcmp(ctx->work_fb, ctx->last_fb, ctx->fb_size) == 0) return 3;

    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    const esp_err_t err = PartialBwBasePrevCurr(ctx, ctx->last_fb, ctx->work_fb, "standby_clock");
    ctx->suppress_lvgl_notify = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "standby clock refresh: %s", esp_err_to_name(err));
        if (err_out) *err_out = err;
        return 2;
    }
    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    ESP_LOGI(TAG, "standby clock %02d:%02d", hh, mm);
    return 0;
}

// ---- 看图灰度波形（可调）
namespace {
int s_lut_ov[5] = {2, 5, 0, 0, 0x22};      // 叠加：深灰 / 浅灰 提亮帧数，fr
int s_lut_full[5] = {40, 3, 16, 40, 0x44};  // 一次刷：清屏 / 浅灰 / 深灰 / 黑 加深帧数，fr（0x44 短帧≈半帧，浅灰 3 短帧=1.5 帧，用户实测）
// 一次刷的浅灰「半帧」微调：组 2 只推浅灰那一行，用更快的帧率（0x44 每帧约 10ms，是 0x22 的一半）
int s_lut_fine[2] = {0, 0x44};
int s_ov_vcom = 0x30;  // 叠加那一遍的 VCOM（原来跟阅读灰 LUT 一样 0x30）
// 刷底方案的叠加方式：0 = 黑底往白推（原方案，串扰明显）；1 = 白底往黑推（和一次刷第二段同一个方向）
int s_ov_style = 1;  // 2.1.47 起默认白底加深（用户实测串扰轻）
int s_ov_dark[3] = {6, 2, 0x44};  // 用户实测：深灰 6 / 浅灰 2 短帧
int s_ov_black = 4;    // 白底加深时给纯黑像素补压的短帧数（行 1），把真灰 0 压得更黑
int s_ov_vsh1 = 0x32;  // 白底加深这一遍的 VSH1（源极正压）；调低推得慢，深浅更好分档
int s_clear_cycles = 1;                    // 一次刷清屏：黑→白 来回几次（0 = 不清，直接画，可能有残影）
constexpr uint8_t kVSS = 0, kVSH1 = 1, kVSL = 2;

void LutSetPhase(uint8_t* lut, int row, int group, int phase, uint8_t v) {
    uint8_t& b = lut[row * 10 + group];
    const int sh = 6 - 2 * phase;
    b = static_cast<uint8_t>((b & ~(3 << sh)) | (v << sh));
}
void LutSetTp(uint8_t* lut, int group, int phase, int frames) {
    lut[50 + group * 5 + phase] = static_cast<uint8_t>(frames < 0 ? 0 : (frames > 255 ? 255 : frames));
}
void LutTail(uint8_t* lut, int fr) {
    for (int i = 0; i < 5; ++i) lut[100 + i] = static_cast<uint8_t>(fr);
    // 电压同阅读灰 LUT：VGH / VSH1 / VSH2 / VSL / VCOM
    lut[105] = 0x17; lut[106] = 0x41; lut[107] = 0xA8; lut[108] = 0x32; lut[109] = 0x30;
}
/** 叠加：底已是黑（灰像素画成黑），行 2 = 浅灰、行 3 = 深灰，VSL 往白推 */
void BuildOverlayLut(uint8_t* lut) {
    memset(lut, 0, 112);
    if (s_ov_style == 1) {
        // 白底：灰像素在底图里是白的；行 2 = 浅灰、行 3 = 深灰，VSH1 往黑推，深灰推得久
        const int dk = s_ov_dark[0], lt = s_ov_dark[1];
        LutSetPhase(lut, 2, 0, 0, kVSH1);
        LutSetPhase(lut, 3, 0, 0, kVSH1);
        LutSetTp(lut, 0, 0, lt < dk ? lt : dk);
        if (dk > lt) {
            LutSetPhase(lut, 3, 0, 1, kVSH1);
            LutSetTp(lut, 0, 1, dk - lt);
        }
        if (s_ov_black > 0) {  // 组 1：纯黑再补压
            LutSetPhase(lut, 1, 1, 0, kVSH1);
            LutSetTp(lut, 1, 0, s_ov_black);
        }
        LutTail(lut, s_ov_dark[2]);
        lut[106] = static_cast<uint8_t>(s_ov_vsh1);
        lut[109] = static_cast<uint8_t>(s_ov_vcom);
        return;
    }
    const int d = s_lut_ov[0], l = s_lut_ov[1];
    LutSetPhase(lut, 2, 0, 0, kVSL);
    LutSetPhase(lut, 3, 0, 0, kVSL);
    LutSetTp(lut, 0, 0, d < l ? d : l);
    if (l > d) {
        LutSetPhase(lut, 2, 0, 1, kVSL);
        LutSetTp(lut, 0, 1, l - d);
    } else if (d > l) {
        LutSetPhase(lut, 3, 0, 1, kVSL);
        LutSetTp(lut, 0, 1, d - l);
    }
    LutTail(lut, s_lut_ov[4]);
    lut[109] = static_cast<uint8_t>(s_ov_vcom);
}
/** 一次刷：行号 n = (0x26 位 << 1) | 0x24 位；0 白 1 浅灰 2 深灰 3 黑 */
void BuildFullLut(uint8_t* lut) {
    memset(lut, 0, 112);
    const int c = s_lut_full[0], tl = s_lut_full[1], td = s_lut_full[2], tb = s_lut_full[3];
    // 组 0：先黑后白清掉上一张的残影（s_clear_cycles 次）；0 次就只把全部推到白再画
    for (int k = 0; k < 2; ++k) {
        if (k >= s_clear_cycles && !(k == 0 && s_clear_cycles == 0)) break;
        for (int r = 0; r < 4; ++r) {
            if (s_clear_cycles > 0) LutSetPhase(lut, r, 0, k * 2, kVSH1);
            LutSetPhase(lut, r, 0, k * 2 + 1, kVSL);
        }
        if (s_clear_cycles > 0) LutSetTp(lut, 0, k * 2, c);
        LutSetTp(lut, 0, k * 2 + 1, c);
    }
    // 组 1：从白往黑推，越黑推越久
    for (int r = 1; r < 4; ++r) LutSetPhase(lut, r, 1, 0, kVSH1);
    LutSetTp(lut, 1, 0, tl);
    for (int r = 2; r < 4; ++r) LutSetPhase(lut, r, 1, 1, kVSH1);
    LutSetTp(lut, 1, 1, td - tl);
    LutSetPhase(lut, 3, 1, 2, kVSH1);
    LutSetTp(lut, 1, 2, tb - td);
    (void)kVSS;
    LutTail(lut, s_lut_full[4]);
    if (s_lut_fine[0] > 0) {  // 组 2：浅灰再补几个短帧
        LutSetPhase(lut, 1, 2, 0, kVSH1);
        LutSetTp(lut, 2, 0, s_lut_fine[0]);
        lut[101] = static_cast<uint8_t>(s_lut_fine[1]);  // 组 2/3 的帧率
    }
}
inline void PanelBit(uint8_t* plane, int stride, int px, int py, bool on) {
    uint8_t& b = plane[py * stride + (px >> 3)];
    const uint8_t m = static_cast<uint8_t>(0x80 >> (px & 7));
    b = on ? static_cast<uint8_t>(b | m) : static_cast<uint8_t>(b & ~m);
}
/**
 * 灰像素在帧缓冲 / 面板 RAM 里记成白：局刷 LUT 对「黑→黑」会再压一下黑（灰会被慢慢压成黑），
 * 对「白→白」不驱动，这样状态栏时钟之类的局刷不会把灰度吃掉。
 */
void WhitenGrayInFb(uint8_t* fb, int panel_w, int panel_h, const uint8_t* g2, int x0, int y0, int w, int h) {
    const int stride = panel_w / 8, stride2 = (w + 3) / 4;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = g2 + static_cast<size_t>(y) * stride2;
        for (int x = 0; x < w; ++x) {
            const int v = (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
            if (v != 1 && v != 2) continue;
            int px = 0, py = 0;
            DisplayUiLogicalToPanel(x0 + x, y0 + y, &px, &py);
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) continue;
            PanelBit(fb, stride, px, py, true);
        }
    }
}
}  // namespace

std::string LVAdapterDisplay::DebugInfo() {
    DisplayLockGuard lock(this);
    EpdFlushCtx* c = epd_flush_ctx_;
    if (c == nullptr) return "no epd ctx";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "vpark=%u "
                  "partials=%u.%03u/%u last_busy=%ums since_last_partial=%dms inflight=%d coalesce=%d gray_hold=%d "
                  "orient=%d press_inv=%d analog=%d last22=%02X",
                  s_vcom_parks, (unsigned)c->partial_refresh_count, (unsigned)c->partial_weight_milli,
                  (unsigned)(PartialsPerFull() == UINT32_MAX ? 0 : PartialsPerFull()),
                  (unsigned)epaper_panel_last_busy_ms(c->panel),
                  c->last_partial_us ? (int)((esp_timer_get_time() - c->last_partial_us) / 1000) : -1,
                  c->refresh_inflight ? 1 : 0, c->coalesce_pending ? 1 : 0, c->gray_hold ? 1 : 0,
                  DisplayUiGetOrient(), s_inv_on ? 1 : 0, epaper_panel_analog_on(c->panel) ? 1 : 0,
                  epaper_panel_last_ctrl2(c->panel));
    return buf;
}

void LVAdapterDisplay::DebugTap(int x, int y, int hold_ms) {
    struct Arg { int x, y, ms; };
    auto* arg = new Arg{x, y, hold_ms};
    xTaskCreate([](void* p) {
        auto* a = static_cast<Arg*>(p);
        s_touch_mark_us = esp_timer_get_time();
        PowerPolicy::GetInstance().NotifyUserActivity();
        portENTER_CRITICAL(&s_touch_snap_mux);
        s_touch_snap.suppress_lvgl = false;
        TouchFeedUiPressLocked(static_cast<uint16_t>(a->x), static_cast<uint16_t>(a->y));
        portEXIT_CRITICAL(&s_touch_snap_mux);
        TouchFeedKickLvgl("dbg_down", 0, 0, 1, false, a->x, a->y);
        s_dbg_drag.store(true);  // 模拟按住期间，真触摸（没按 = 抬起）别喂钩子
        if (auto hook = s_touch_raw_hook.load()) hook(true, a->x, a->y);
        vTaskDelay(pdMS_TO_TICKS(a->ms));
        if (auto hook = s_touch_raw_hook.load()) hook(false, a->x, a->y);
        s_dbg_drag.store(false);
        portENTER_CRITICAL(&s_touch_snap_mux);
        TouchFeedUiReleaseLocked();
        portEXIT_CRITICAL(&s_touch_snap_mux);
        TouchFeedKickLvgl("dbg_up", 0, 0, 0, true, a->x, a->y);
        delete a;
        vTaskDelete(nullptr);
    }, "dbg_tap", 3072, arg, 5, nullptr);
}

void LVAdapterDisplay::SetGrayLut(int which, int a, int b, int c, int d, int fr) {
    if (which == 4) {  // 普通局刷：a=组0帧数 b=组1帧数 c=帧率字节（<=0 不改）
        if (a > 0) s_norm_tp0 = a;
        if (b >= 0) s_norm_tp1 = b;
        epaper_panel_set_partial_timing(s_epd_panel, a, b, c);
        return;
    }
    if (which == 9) {  // 白底加深：纯黑补压短帧数
        s_ov_black = a < 0 ? 0 : a;
        ESP_LOGW(TAG, "overlay black push = %d", s_ov_black);
        return;
    }
    if (which == 8) {  // 刷底叠加方式：a=0 黑底提亮 / 1 白底加深；b,c = 白底加深的深灰/浅灰帧数；d = 帧率
        s_ov_style = a;
        if (b > 0) s_ov_dark[0] = b;
        if (c > 0) s_ov_dark[1] = c;
        if (d > 0) s_ov_dark[2] = d;
        if (fr > 0) s_ov_vsh1 = fr;
        ESP_LOGW(TAG, "overlay style=%d dark=%d light=%d fr=0x%02X vsh1=0x%02X", s_ov_style, s_ov_dark[0], s_ov_dark[1],
                 s_ov_dark[2], s_ov_vsh1);
        return;
    }
    if (which == 7) {  // 叠加那一遍的 VCOM
        s_ov_vcom = a & 0xFF;
        ESP_LOGW(TAG, "overlay vcom = 0x%02X", s_ov_vcom);
        return;
    }
    if (which == 6) {  // 一次刷浅灰微调：a=短帧数 b=帧率字节
        s_lut_fine[0] = a < 0 ? 0 : a;
        if (b > 0) s_lut_fine[1] = b;
        ESP_LOGW(TAG, "gray fine = %d fr=0x%02X", s_lut_fine[0], s_lut_fine[1]);
        return;
    }
    if (which == 5) {  // 按下反色的短波形帧数
        s_press_tp0 = a < 0 ? 0 : a;
        ESP_LOGW(TAG, "press flash tp0=%d", s_press_tp0);
        return;
    }
    if (which == 3) {  // 看图保灰：1 = 保持型局刷波形照常刷状态栏，0 = 状态栏攒着不刷
        s_gray_hold_lut = a != 0;
        ESP_LOGW(TAG, "gray hold lut = %d", a);
        return;
    }
    if (which == 2) {  // 清屏次数
        s_clear_cycles = a < 0 ? 0 : (a > 2 ? 2 : a);
        ESP_LOGW(TAG, "gray clear cycles = %d", s_clear_cycles);
        return;
    }
    int* t = which == 0 ? s_lut_ov : s_lut_full;
    t[0] = a; t[1] = b; t[2] = c; t[3] = d;
    if (fr > 0) t[4] = fr;
    ESP_LOGW(TAG, "gray lut %d = %d %d %d %d fr=0x%02X", which, t[0], t[1], t[2], t[3], t[4]);
}

bool LVAdapterDisplay::GrayOverlayFromWhite() { return s_ov_style == 1; }

void LVAdapterDisplay::DebugRawDrag(int x0, int y0, int x1, int y1, int ms) {
    struct Arg { int x0, y0, x1, y1, ms; };
    auto* arg = new Arg{x0, y0, x1, y1, ms};
    xTaskCreate([](void* p) {
        auto* a = static_cast<Arg*>(p);
        s_dbg_drag.store(true);
        const int n = a->ms / 10 > 1 ? a->ms / 10 : 2;
        for (int i = 0; i <= n; ++i) {
            if (auto hook = s_touch_raw_hook.load()) hook(true, a->x0 + (a->x1 - a->x0) * i / n, a->y0 + (a->y1 - a->y0) * i / n);
            vTaskDelay(1);
        }
        if (auto hook = s_touch_raw_hook.load()) hook(false, a->x1, a->y1);
        s_dbg_drag.store(false);
        delete a;
        vTaskDelete(nullptr);
    }, "dbg_drag", 3072, arg, 5, nullptr);
}

void LVAdapterDisplay::SetTouchRawHook(void (*hook)(bool down, int x, int y), int period_ms) {
    s_touch_raw_hook.store(hook);
    s_touch_period_ms.store(period_ms > 0 ? period_ms : kTouchFeedPeriodMs);
}

void LVAdapterDisplay::GetGrayLut(int which, int out[5]) {
    const int* t = which == 0 ? s_lut_ov : s_lut_full;
    for (int i = 0; i < 5; ++i) out[i] = t[i];
}

void LVAdapterDisplay::SetGrayHold(bool on, int top_h) {
    DisplayLockGuard lock(this);
    if (epd_flush_ctx_ == nullptr) return;
    ESP_LOGI(TAG, "gray hold %d h=%d", on ? 1 : 0, top_h);
    epd_flush_ctx_->gray_hold_h = top_h;
    epd_flush_ctx_->gray_hold = on;
}

void LVAdapterDisplay::SetAbsorbFlush(bool on) {
    DisplayLockGuard lock(this);
    if (epd_flush_ctx_) epd_flush_ctx_->absorb_flush = on;
}

int LVAdapterDisplay::ApplyGray4Single(const uint8_t* g2, int x0, int y0, int w, int h, int timeout_ms) {
    if (g2 == nullptr || w <= 0 || h <= 0) return 1;
    (void)timeout_ms;
    DisplayLockGuard lock(this);
    auto* ctx = epd_flush_ctx_;
    if (ctx == nullptr || ctx->panel == nullptr || ctx->gray_scratch == nullptr || ctx->freeze_updates) return 2;
    DrainRefreshIfNeeded(ctx);
    ctx->absorb_flush = false;
    if (!ctx->coalesce_pending) memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
    ctx->coalesce_pending = false;
    ctx->coalesce_kick_armed = false;
    const int stride = ctx->panel_w / 8;
    const int stride2 = (w + 3) / 4;
    // 两个平面：底图外的像素按帧缓冲（黑→3 白→0），图内按灰度（g2 0 黑..3 白 → n = 3 - v）
    uint8_t* planes[2] = {ctx->gray_scratch, ctx->last_fb};  // last_fb 临时借来当第二平面，最后用 work_fb 恢复
    for (int pl = 0; pl < 2; ++pl) {
        uint8_t* out = planes[pl];
        for (size_t i = 0; i < ctx->fb_size; ++i) out[i] = static_cast<uint8_t>(~ctx->work_fb[i]);
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = g2 + static_cast<size_t>(y) * stride2;
            for (int x = 0; x < w; ++x) {
                const int v = (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
                const int n = 3 - v;
                int px = 0, py = 0;
                DisplayUiLogicalToPanel(x0 + x, y0 + y, &px, &py);
                if (px < 0 || py < 0 || px >= ctx->panel_w || py >= ctx->panel_h) continue;
                PanelBit(out, stride, px, py, pl == 0 ? (n & 1) : (n >> 1));
            }
        }
    }
    static uint8_t lut[112];
    BuildFullLut(lut);
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = epaper_panel_load_custom_lut(ctx->panel, lut);
    if (err == ESP_OK) {
        epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
        err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
    }
    if (err == ESP_OK) {
        epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
        err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->last_fb);
    }
    if (err == ESP_OK) {
        epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_CUSTOM);
        err = epaper_panel_refresh_screen(ctx->panel);
    }
    const unsigned busy = epaper_panel_last_busy_ms(ctx->panel);
    // 面板 RAM 和帧缓冲都回到黑白底（灰像素当黑），之后局刷照常
    WhitenGrayInFb(ctx->work_fb, ctx->panel_w, ctx->panel_h, g2, x0, y0, w, h);
    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, "gray4_single");
    xSemaphoreTake(ctx->done_sem, 0);
    ctx->suppress_lvgl_notify = false;
    ctx->has_last = true;
    ctx->partial_refresh_count = 0;
    ctx->partial_weight_milli = 0;
    ctx->last_partial_us = esp_timer_get_time();  // 出灰后别马上被空闲掉电
    ESP_LOGI(TAG, "gray4 single %dx%d busy=%ums total=%dms -> %s", w, h, busy,
             static_cast<int>((esp_timer_get_time() - t0) / 1000), esp_err_to_name(err));
    return err == ESP_OK ? 0 : 3;
}

int LVAdapterDisplay::ApplyGray4Overlay(const uint8_t* g2, int x0, int y0, int w, int h, int timeout_ms) {
    if (g2 == nullptr || w <= 0 || h <= 0) return 1;
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    for (;;) {
        {
            DisplayLockGuard lock(this);
            auto* ctx = epd_flush_ctx_;
            if (ctx == nullptr || ctx->panel == nullptr || ctx->gray_scratch == nullptr || ctx->freeze_updates) return 2;
            DrainRefreshIfNeeded(ctx);
            if (!ctx->coalesce_pending && !ctx->defer_standby_paint && ctx->has_last) {
                // 底图已上屏：work_fb = 屏上的黑白底，灰步收尾会用它把面板 RAM 写回 BW
                memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
                epd_gray_aa_clear();
                const int stride = (w + 3) / 4;
                int ngray = 0;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* row = g2 + static_cast<size_t>(y) * stride;
                    for (int x = 0; x < w; ++x) {
                        const int v = (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
                        if (v == 1 || v == 2) {
                            epd_gray_aa_set_logical(x0 + x, y0 + y, v == 1 ? 170 : 85);
                            ++ngray;
                        } else if (v == 0 && s_ov_style == 1 && s_ov_black > 0) {
                            epd_gray_aa_set_logical(x0 + x, y0 + y, 255);  // 纯黑补压（只进 LSB 平面）
                        }
                    }
                }
                ctx->suppress_lvgl_notify = true;
                xSemaphoreTake(ctx->done_sem, 0);
                esp_err_t err;
                if (s_lut_ov[0] == 0 && s_lut_ov[1] == 0) {
                    err = GrayOverlayAfterBwBase(ctx, "gray4_img");  // 阅读器原表
                } else {
                    static uint8_t lut[112];
                    BuildOverlayLut(lut);
                    err = epaper_panel_load_custom_lut(ctx->panel, lut);
                    if (err == ESP_OK) {
                        epd_gray_aa_pack_overlay_plane_ex(ctx->gray_scratch, 0, s_ov_style == 1 && s_ov_black > 0);
                        epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_CURRENT);
                        err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
                    }
                    if (err == ESP_OK) {
                        epd_gray_aa_pack_overlay_plane(ctx->gray_scratch, 1);
                        epaper_panel_set_bitmap_color(ctx->panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
                        err = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->gray_scratch);
                    }
                    if (err == ESP_OK) {
                        epaper_panel_set_refresh_mode(ctx->panel, SSD1677_EPAPER_REFRESH_CUSTOM);
                        err = epaper_panel_refresh_screen(ctx->panel);
                    }
                    WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, "gray4_img");
                }
                WhitenGrayInFb(ctx->work_fb, ctx->panel_w, ctx->panel_h, g2, x0, y0, w, h);
                memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
                WriteBothPlanes(ctx, ctx->work_fb, ctx->work_fb, "gray4_img_fix");
                xSemaphoreTake(ctx->done_sem, 0);
                ctx->suppress_lvgl_notify = false;
                epd_gray_aa_clear();
                ctx->last_partial_us = esp_timer_get_time();  // 出灰后别马上被空闲掉电
                ESP_LOGI(TAG, "gray4 overlay %dx%d gray=%d -> %s", w, h, ngray, esp_err_to_name(err));
                return err == ESP_OK ? 0 : 3;
            }
        }
        if (esp_timer_get_time() > deadline) return 4;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void LVAdapterDisplay::RequestNextFullRefresh() {
    if (epd_flush_ctx_ == nullptr) {
        return;
    }
    epd_flush_ctx_->force_next_full = true;
}

void LVAdapterDisplay::RequestDualPlanePartial() {
    if (epd_flush_ctx_ == nullptr) {
        return;
    }
    epd_flush_ctx_->force_dual_plane_partial = true;
    // 与 C4 互斥：开/关 AA 要的是局刷双平面，不是全刷
    epd_flush_ctx_->force_next_full = false;
    epd_flush_ctx_->force_full_c7 = false;
}

void LVAdapterDisplay::SleepEpdForPowerOff() {
    if (epd_flush_ctx_ == nullptr || epd_flush_ctx_->panel == nullptr) {
        return;
    }
    DrainRefreshIfNeeded(epd_flush_ctx_);
    const esp_err_t err = epaper_panel_deep_sleep(epd_flush_ctx_->panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EPD deep sleep: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGD(TAG, "EPD deep sleep before PWR_KEY");
}

void LVAdapterDisplay::ShowPoweredOffScreen() {
    // 若待机曾冻结 flush，先恢复再画关机图
    WakeEpdFromStandby();
    DisplayLockGuard lock(this);
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    // 直接往 EPD 帧缓冲画 A2I1，再关机全刷（0xC7）。
    // 加载链：NVS 源文件 →（失败/坏图）内置 mmap →（再失败）「已关机」文字
    bool have_img = false;
    uint8_t* sd_buf = nullptr;
    size_t sd_len = 0;
    if (epd_flush_ctx_ != nullptr && epd_flush_ctx_->work_fb != nullptr) {
        const uint8_t* mem = nullptr;
        size_t plen = 0;
        bool from_sd = TryLoadShutdownA2i1FromSd(&sd_buf, &sd_len);

        auto try_parse = [&](const uint8_t* raw, size_t raw_len, bool strip_mmap_prefix) -> bool {
            if (raw == nullptr || raw_len == 0) {
                return false;
            }
            const uint8_t* p = raw;
            size_t n = raw_len;
            if (strip_mmap_prefix && n >= 6 && p[0] == 0x5A && p[1] == 0x5A && p[2] == 'A' &&
                p[3] == '2' && p[4] == 'I' && p[5] == '1') {
                p += 2;
                n -= 2;
            }
            uint16_t w = 0, h = 0, stride = 0;
            const uint8_t* payload = nullptr;
            if (!ParseA2i1(p, n, &w, &h, &stride, &payload) || payload == nullptr) {
                return false;
            }
            BlitA2i1ToPanelFb(epd_flush_ctx_->work_fb, epd_flush_ctx_->panel_w,
                              epd_flush_ctx_->panel_h, payload, w, h, stride);
            OverlayShutdownHint(epd_flush_ctx_->work_fb, epd_flush_ctx_->panel_w, epd_flush_ctx_->panel_h);
            const esp_err_t err = EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->work_fb);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "shutdown full refresh failed: %s", esp_err_to_name(err));
                return false;
            }
            return true;
        };

        if (from_sd) {
            if (try_parse(sd_buf, sd_len, false)) {
                have_img = true;
            } else {
                ESP_LOGW(TAG, "shutdown img: SD A2I1 unusable → built-in");
                heap_caps_free(sd_buf);
                sd_buf = nullptr;
                sd_len = 0;
                from_sd = false;
            }
        }

        if (!have_img && resources_assets_ != nullptr) {
            mem = mmap_assets_get_mem(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            const int sz = mmap_assets_get_size(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            if (mem != nullptr && sz > 0) {
                plen = static_cast<size_t>(sz);
                ESP_LOGI(TAG, "shutdown img: use built-in bg_shutdown.a2i1 (%u bytes)",
                         static_cast<unsigned>(plen));
                have_img = try_parse(mem, plen, true);
                if (!have_img) {
                    ESP_LOGW(TAG, "shutdown img: built-in A2I1 parse/blit fail");
                }
            } else {
                ESP_LOGW(TAG, "shutdown img: built-in missing mem=%p sz=%d", mem, sz);
            }
        } else if (!have_img) {
            ESP_LOGW(TAG, "shutdown img: no resources_assets_");
        }
    }
    if (sd_buf != nullptr) {
        heap_caps_free(sd_buf);
        sd_buf = nullptr;
    }

    if (have_img) {
        // 冻结后续 flush，禁止 LVGL 再把白屏刷上去。
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
    } else {
        lv_obj_t* scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, Lang::Strings::POWERED_OFF);
        const lv_font_t* font = fontpack_lv_font_get(30, 2);
        if (font == nullptr) {
            font = fontpack_lv_font_ui();
        }
        if (font != nullptr) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        lv_obj_t* old_scr = lv_screen_active();
        lv_screen_load(scr);
        if (old_scr != nullptr && old_scr != scr) {
            lv_obj_delete(old_scr);
        }
        lv_obj_invalidate(scr);
        lv_refr_now(nullptr);
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
        ESP_LOGI(TAG, "shutdown img: fallback text POWERED_OFF");
    }
    ESP_LOGD(TAG, "powered-off screen shown (a2i1=%d)", have_img ? 1 : 0);
}

void LVAdapterDisplay::ShowLowBatteryShutdownScreen() {
    WakeEpdFromStandby();
    DisplayLockGuard lock(this);
    EpdFlushCtx* ctx = epd_flush_ctx_;
    if (ctx != nullptr) {
        DrainRefreshIfNeeded(ctx);
        ctx->defer_boot_paint = false;
        ctx->defer_standby_paint = false;
        ctx->coalesce_pending = false;
        ctx->coalesce_kick_armed = false;
        if (ctx->coalesce_timer != nullptr) {
            esp_timer_stop(ctx->coalesce_timer);
        }
    }
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(scr);
    lv_label_set_text(lbl, Lang::Strings::BATTERY_UV_SHUTDOWN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_HOR_RES * 9 / 10);
    const lv_font_t* font = fontpack_lv_font_get(30, 2);
    if (font == nullptr) {
        font = fontpack_lv_font_ui();
    }
    if (font != nullptr) {
        lv_obj_set_style_text_font(lbl, font, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_t* old_scr = lv_screen_active();
    lv_screen_load(scr);
    if (old_scr != nullptr && old_scr != scr) {
        lv_obj_delete(old_scr);
    }
    lv_obj_invalidate(scr);
    lv_refr_now(nullptr);

    // lv_refr 在 BUSY/coalesce/defer 时可能只攒帧；须同步 FULL 上屏后再 freeze（对齐 ParkEpd / 关机图）
    if (ctx != nullptr && ctx->panel != nullptr && ctx->last_fb != nullptr) {
        DrainRefreshIfNeeded(ctx);
        const uint8_t* fb = ctx->last_fb;
        if (ctx->coalesce_pending && ctx->work_fb != nullptr) {
            fb = ctx->work_fb;
        }
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        ESP_LOGD(TAG, "low-batt warn: FULL 0xC7");
        const esp_err_t err =
            FullRefreshBothSame(ctx, fb, "low_batt_warn", SSD1677_EPAPER_REFRESH_STANDBY);
        ctx->suppress_lvgl_notify = false;
        if (err == ESP_OK) {
            if (fb != ctx->last_fb) {
                memcpy(ctx->last_fb, fb, ctx->fb_size);
            }
            ctx->has_last = true;
            ctx->partial_refresh_count = 0;
        ctx->partial_weight_milli = 0;
            ctx->coalesce_pending = false;
        } else {
            ESP_LOGW(TAG, "low-batt warn FULL: %s", esp_err_to_name(err));
        }
        DrainRefreshIfNeeded(ctx);
    }
    if (ctx != nullptr) {
        ctx->freeze_updates = true;
    }
    ESP_LOGD(TAG, "low-battery shutdown warning shown");
}

void LVAdapterDisplay::ApplyStatusTextLocked(const char* status) {
    const char* body = (status != nullptr) ? status : "";
    if (body != status_body_cache_) {
        std::snprintf(status_body_cache_, sizeof(status_body_cache_), "%s", body);
    }
    if (status_label_ == nullptr) {
        return;
    }
    if (status_title_prefix_[0] != '\0') {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s·%s", status_title_prefix_, status_body_cache_);
        lv_label_set_text(status_label_, buf);
    } else {
        lv_label_set_text(status_label_, status_body_cache_);
    }
}

void LVAdapterDisplay::SetStatus(const char* status) {
    // 百问页：屏蔽待命/聆听/回答，保留连接网络/登录等系统提示（跟 DeviceState）
    if (AssistantScreen::IsActive()) {
        const DeviceState state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateIdle || state == kDeviceStateListening ||
            state == kDeviceStateSpeaking) {
            return;
        }
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        return;
    }
    ApplyStatusTextLocked(status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    if (status_label_) {
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    // 音量键通知后同步顶栏静音图标
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        const bool want_mute = (codec->output_volume() == 0);
        if (want_mute != muted_ && mute_label_ != nullptr) {
            muted_ = want_mute;
            lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
        }
    }
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000ULL));
    }
}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {
    // 全屏页（老化/关机图等）已 ClearStatusBindings：勿再抢 DisplayLock，减轻与 LVGL 竞态
    if (mute_label_ == nullptr && network_label_ == nullptr && battery_label_ == nullptr &&
        status_label_ == nullptr && notification_label_ == nullptr && battery_pct_label_ == nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    // I2C / 业务读取放锁外；所有 label 改动合并进一次 DisplayLock，减少无效局刷。
    const bool want_mute = (codec != nullptr && codec->output_volume() == 0);

    char time_str[16] = {};
    bool set_time = false;
    const bool assistant_page = AssistantScreen::IsActive();
    const auto device_state = app.GetDeviceState();
    // 百问联网/启动中：顶栏保留「连接中…」等，勿用时钟盖住；Idle/聆听/播报仍刷时钟（对话文案已在 SetStatus 过滤）
    const bool assistant_net_hint =
        assistant_page && (device_state == kDeviceStateConnecting ||
                           device_state == kDeviceStateStarting ||
                           device_state == kDeviceStateWifiConfiguring ||
                           device_state == kDeviceStateActivating);
    const bool want_clock = AllowsIdleStatusClock() && !assistant_net_hint &&
                            (assistant_page ||
                             (!app.HasPendingActivationCode() && !app.IsXiaozhiVoiceActive() &&
                              device_state == kDeviceStateIdle));
    if (want_clock &&
        (update_all || last_status_update_time_ + std::chrono::seconds(10) <
                            std::chrono::system_clock::now())) {
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && tm->tm_year >= 2025 - 1900) {
            strftime(time_str, sizeof(time_str), "%H:%M", tm);
            set_time = true;
        }
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* new_battery_icon = nullptr;
    const bool battery_ok = board.GetBatteryLevel(battery_level, charging, discharging);
    if (battery_ok) {
        if (charging) {
            new_battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            static const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, FONT_AWESOME_BATTERY_QUARTER, FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS, FONT_AWESOME_BATTERY_FULL, FONT_AWESOME_BATTERY_FULL,
            };
            new_battery_icon = levels[battery_level / 20];
        }
    }

    const char* new_network_icon = nullptr;
    static int seconds_counter = 0;
    const bool wifi_busy = WifiStation::GetInstance().IsLinkInProgress();
    // 常态约 10s；WiFi 扫描/连接中每秒刷，配合弱→中→强梯度
    if (update_all || wifi_busy || (++seconds_counter % 10 == 0)) {
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,       kDeviceStateStarting,   kDeviceStateWifiConfiguring,
            kDeviceStateConnecting, kDeviceStateListening,  kDeviceStateSpeaking,
            kDeviceStateActivating,
        };
        if (update_all || wifi_busy ||
            std::find(allowed_states.begin(), allowed_states.end(), device_state) !=
                allowed_states.end()) {
            new_network_icon = board.GetNetworkStateIcon();
        }
    }

    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        if (want_mute != muted_) {
            muted_ = want_mute;
            lv_label_set_text(mute_label_, muted_ ? FONT_AWESOME_VOLUME_XMARK : "");
        }

        if (set_time && status_label_ != nullptr) {
            ApplyStatusTextLocked(time_str);
            // 百问 PTT 波形占用状态栏中央时只更新文案，勿揭开时钟盖住动画
            // 通知展示中同样勿用时钟盖住（如开机 IO 扩展异常）
            if (!AssistantScreen::IsPttWaveVisible()) {
                const bool notif_on =
                    notification_label_ != nullptr &&
                    !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                if (!notif_on) {
                    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
                    if (notification_label_ != nullptr) {
                        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
            last_status_update_time_ = std::chrono::system_clock::now();
        }

        if (battery_ok && new_battery_icon != nullptr) {
            if (battery_icon_ != new_battery_icon) {
                battery_icon_ = new_battery_icon;
                if (battery_label_ != nullptr) {
                    lv_label_set_text(battery_label_, battery_icon_);
                }
            }
            if (battery_level != battery_level_cache_) {
                battery_level_cache_ = battery_level;
                snprintf(battery_pct_cache_, sizeof(battery_pct_cache_), "%d%%", battery_level);
                if (battery_pct_label_ != nullptr) {
                    lv_label_set_text(battery_pct_label_, battery_pct_cache_);
                }
            }
            if (low_battery_popup_ != nullptr) {
                if (std::strcmp(new_battery_icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                    if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    }
                } else if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        if (new_network_icon != nullptr && network_icon_ != new_network_icon) {
            network_icon_ = new_network_icon;
            if (network_label_ != nullptr) {
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }
}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

void LVAdapterDisplay::SetTheme(Theme* theme) {
    Display::SetTheme(theme);
}

bool LVAdapterDisplay::Lock(int timeout_ms) {
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void LVAdapterDisplay::Unlock() {
    esp_lv_adapter_unlock();
}
