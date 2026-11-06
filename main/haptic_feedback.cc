#include "haptic_feedback.h"

#include "board.h"
#include "settings.h"

#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "Haptic";
constexpr const char* kNvsNs = "display";
constexpr const char* kNvsKey = "haptic";
constexpr const char* kNvsIntensityKey = "haptic_lvl";   // 旧：0 弱 / 1 中 / 2 强
constexpr const char* kNvsLevelKey = "haptic_lv7";        // 新：7 档 0..6
constexpr bool kDefaultEnabled = true;
// 7 档震动：档位越高脉宽越长（马达转得越久，手感越重）
constexpr int kPulseMs[HAPTIC_LEVELS] = {18, 25, 32, 40, 50, 60, 72};
constexpr int kDefaultIntensity = 2;  // 32ms，接近原来的「弱」

// LVGL USER_1：标记已挂震动回调，避免重复 Attach 导致连震。
constexpr lv_obj_flag_t kHapticBoundFlag = LV_OBJ_FLAG_USER_1;
constexpr size_t kMaxZones = 96;

struct HapticZone {
    lv_obj_t* obj = nullptr;
    int16_t x1 = 0;
    int16_t y1 = 0;
    int16_t x2 = 0;
    int16_t y2 = 0;
};

bool s_enabled = kDefaultEnabled;
int s_intensity = kDefaultIntensity;
bool s_ready = false;
bool s_zone_early_pulse = true;  // 全屏遮罩时可关，避免下层热区误震
// touch_feed 早震与 LVGL PRESSED 去重
uint32_t s_press_seq = 0;
uint32_t s_pulsed_seq = 0;

HapticZone s_zones[kMaxZones]{};
size_t s_zone_count = 0;
portMUX_TYPE s_zone_mux = portMUX_INITIALIZER_UNLOCKED;

int ClampIntensity(int intensity) {
    return intensity < 0 ? 0 : (intensity >= HAPTIC_LEVELS ? HAPTIC_LEVELS - 1 : intensity);
}

int PulseMsForIntensity(int intensity) { return kPulseMs[ClampIntensity(intensity)]; }

void LoadFromNvs() {
    Settings settings(kNvsNs, false);
    s_enabled = settings.GetBool(kNvsKey, kDefaultEnabled);
    const int lv7 = settings.GetInt(kNvsLevelKey, -1);
    if (lv7 >= 0) {
        s_intensity = ClampIntensity(lv7);
    } else {
        // 从旧的三档迁移：弱→3 档(32ms) 中→5 档(50ms) 强→7 档(72ms)
        static const int kMap[3] = {2, 4, 6};
        const int old = settings.GetInt(kNvsIntensityKey, 0);
        s_intensity = kMap[(old >= 0 && old <= 2) ? old : 0];
    }
    s_ready = true;
    ESP_LOGI(TAG, "nvs load haptic=%d intensity=%d (%dms)", s_enabled ? 1 : 0, s_intensity,
             PulseMsForIntensity(s_intensity));
}

void EnsureReady() {
    if (!s_ready) {
        LoadFromNvs();
    }
}

void PersistTask(void* /*arg*/) {
    Settings settings(kNvsNs, true);
    settings.SetBool(kNvsKey, s_enabled);
    settings.SetInt(kNvsLevelKey, s_intensity);
    ESP_LOGI(TAG, "nvs save haptic=%d intensity=%d", s_enabled ? 1 : 0, s_intensity);
    vTaskDelete(nullptr);
}

void SchedulePersist() {
    // 与 card_style 同理：LVGL worker 栈在 PSRAM，禁止本任务直接写 NVS。
    if (xTaskCreate(PersistTask, "haptic_nvs", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(haptic_nvs) failed");
    }
}

void PulseNow() {
    Board::GetInstance().PulseVibration(PulseMsForIntensity(s_intensity));
}

void WriteZoneCoords(HapticZone* z, lv_obj_t* obj) {
    lv_area_t a{};
    lv_obj_get_coords(obj, &a);
    portENTER_CRITICAL(&s_zone_mux);
    z->obj = obj;
    z->x1 = a.x1;
    z->y1 = a.y1;
    z->x2 = a.x2;
    z->y2 = a.y2;
    portEXIT_CRITICAL(&s_zone_mux);
}

HapticZone* FindZoneByObj(const lv_obj_t* obj) {
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            return &s_zones[i];
        }
    }
    return nullptr;
}

void RemoveZoneAt(size_t idx) {
    if (idx >= s_zone_count) {
        return;
    }
    portENTER_CRITICAL(&s_zone_mux);
    s_zones[idx] = s_zones[s_zone_count - 1];
    s_zones[s_zone_count - 1] = {};
    s_zone_count--;
    portEXIT_CRITICAL(&s_zone_mux);
}

bool RegisterZone(lv_obj_t* obj) {
    HapticZone* existing = FindZoneByObj(obj);
    if (existing != nullptr) {
        WriteZoneCoords(existing, obj);
        return true;
    }
    if (s_zone_count >= kMaxZones) {
        ESP_LOGW(TAG, "zone table full (%u)", static_cast<unsigned>(kMaxZones));
        return false;
    }
    lv_area_t a{};
    lv_obj_get_coords(obj, &a);
    portENTER_CRITICAL(&s_zone_mux);
    HapticZone* z = &s_zones[s_zone_count];
    z->obj = obj;
    z->x1 = a.x1;
    z->y1 = a.y1;
    z->x2 = a.x2;
    z->y2 = a.y2;
    s_zone_count++;
    portEXIT_CRITICAL(&s_zone_mux);
    return true;
}

void OnGeomChanged(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    HapticZone* z = FindZoneByObj(obj);
    if (z == nullptr) {
        return;
    }
    lv_area_t a{};
    lv_obj_get_coords(obj, &a);
    // 仅坐标真变才写：布局改的是位置，SIZE_CHANGED 往往不来，靠 DRAW 补
    if (z->x1 == a.x1 && z->y1 == a.y1 && z->x2 == a.x2 && z->y2 == a.y2) {
        return;
    }
    WriteZoneCoords(z, obj);
}

void OnObjDeleted(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            RemoveZoneAt(i);
            return;
        }
    }
}

void OnPressFeedback(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    HapticZone* z = FindZoneByObj(obj);
    if (z != nullptr) {
        WriteZoneCoords(z, obj);
    }
    if (s_press_seq != 0 && s_pulsed_seq == s_press_seq) {
        return;
    }
    // 占位，防止同一次按下多次 PRESSED / 抬手后晚到的 PRESSED 再震
    if (s_press_seq != 0) {
        s_pulsed_seq = s_press_seq;
    }
    HapticPulseIfEnabled();
}

}  // namespace

bool HapticIsEnabled(void) {
    EnsureReady();
    return s_enabled;
}

void HapticSetEnabled(bool enabled) {
    EnsureReady();
    s_enabled = enabled;
    s_ready = true;
    SchedulePersist();
}

int HapticGetIntensity(void) {
    EnsureReady();
    return s_intensity;
}

void HapticSetIntensity(int intensity) {
    EnsureReady();
    const int next = ClampIntensity(intensity);
    if (s_intensity == next) {
        return;
    }
    s_intensity = next;
    s_ready = true;
    SchedulePersist();
}

void HapticPulseIfEnabled(void) {
    if (!HapticIsEnabled()) {
        return;
    }
    PulseNow();
}

void HapticBeginPress(void) {
    s_press_seq++;
    s_pulsed_seq = 0;
}

void HapticPulseOnFingerDown(void) {
    if (s_press_seq == 0) {
        s_press_seq = 1;
    }
    s_pulsed_seq = s_press_seq;
    if (!HapticIsEnabled()) {
        return;
    }
    PulseNow();
}

bool HapticTryPulseAtUiPoint(int16_t x, int16_t y) {
    if (!HapticIsEnabled() || !s_zone_early_pulse) {
        return false;
    }
    bool hit = false;
    portENTER_CRITICAL(&s_zone_mux);
    for (size_t i = 0; i < s_zone_count; ++i) {
        const HapticZone& z = s_zones[i];
        if (z.obj == nullptr) {
            continue;
        }
        if (x >= z.x1 && x <= z.x2 && y >= z.y1 && y <= z.y2) {
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_zone_mux);
    if (!hit) {
        return false;
    }
    HapticPulseOnFingerDown();
    return true;
}

void HapticSetZoneEarlyPulseEnabled(bool enabled) {
    s_zone_early_pulse = enabled;
}

void HapticAttachClick(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    if (lv_obj_has_flag(obj, kHapticBoundFlag)) {
        return;
    }
    lv_obj_add_flag(obj, kHapticBoundFlag);
    lv_obj_add_event_cb(obj, OnPressFeedback, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, OnGeomChanged, LV_EVENT_SIZE_CHANGED, nullptr);
    // 布局常改位置不改尺寸：SIZE_CHANGED 不来，上屏绘制时刷新热区坐标
    lv_obj_add_event_cb(obj, OnGeomChanged, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(obj, OnObjDeleted, LV_EVENT_DELETE, nullptr);
    RegisterZone(obj);
}

void HapticDetachClick(lv_obj_t* obj) {
    if (obj == nullptr || !lv_obj_has_flag(obj, kHapticBoundFlag)) {
        return;
    }
    lv_obj_clear_flag(obj, kHapticBoundFlag);
    lv_obj_remove_event_cb(obj, OnPressFeedback);
    lv_obj_remove_event_cb(obj, OnGeomChanged);
    lv_obj_remove_event_cb(obj, OnObjDeleted);
    for (size_t i = 0; i < s_zone_count; ++i) {
        if (s_zones[i].obj == obj) {
            RemoveZoneAt(i);
            break;
        }
    }
}
