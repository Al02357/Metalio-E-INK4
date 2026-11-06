#include "net_mode.h"

#include <atomic>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include "board.h"
#include "esp_network.h"
#include "display.h"
#include "settings.h"
#include "wifi_station.h"
#include "display/screen/weread_screen/weread_screen.h"

namespace net_mode {
namespace {
constexpr const char* TAG = "NetMode";
constexpr const char* kNs = "netmode";
constexpr const char* kKey = "offline";
std::atomic<int> s_cached{-1};  // -1 未读
std::atomic<bool> s_yield{false};  // 蓝牙键鼠页临时关 WiFi

void ApplyTask(void* arg) {
    const bool on = arg != nullptr;
    if (on) {
        WifiStation::GetInstance().PauseForLp();  // 停扫、断开
        ESP_LOGW(TAG, "offline mode ON: wifi paused");
    } else if (s_yield.load()) {
        ESP_LOGW(TAG, "offline mode OFF: radio yielded to bluetooth, bring up later");
    } else {
        ESP_LOGW(TAG, "offline mode OFF: bring wifi up");
        if (Board::GetInstance().EnsureNetworkReady(20000)) WeReadScreen::OnNetworkBack();
    }
    if (auto* d = Board::GetInstance().GetDisplay()) d->UpdateStatusBar(true);
    vTaskDelete(nullptr);
}
}  // namespace

bool OfflinePersisted() {
    int v = s_cached.load();
    if (v < 0) {
        Settings s(kNs, false);
        v = s.GetBool(kKey, false) ? 1 : 0;
        s_cached.store(v);
    }
    return v == 1;
}

bool Offline() { return s_yield.load() || OfflinePersisted(); }

bool RadioYielded() { return s_yield.load(); }

void SetRadioYield(bool on) {
    if (s_yield.load() == on) return;
    if (on) {
        s_yield.store(true);
        // 蓝牙要 WiFi 的内部 RAM，必须 stop + deinit。但拆 netif 时后台还有连接在建（微信读书下载、OTA 查询），
        // lwip 当场崩（用户实测：从微信读书出来进蓝牙键鼠就重启）。所以先关网络闸门、停射频，
        // 等后台连接失败退出（最多 1.5 秒），再拆
        EspNetwork_SetBlocked(true);
        WifiStation::GetInstance().PauseForLp();
        int waited = 0;
        while (EspNetwork_LiveSockets() > 0 && waited < 1500) {  // 有的连接对象常驻不放，别等太久
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
        const int live = EspNetwork_LiveSockets();
        vTaskDelay(pdMS_TO_TICKS(200));  // 刚关掉的连接让 lwip 收尾
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK) WifiStation::GetInstance().Stop();
        ESP_LOGW(TAG, "radio yielded: wifi deinit (waited %dms, live sockets %d), internal free=%u", waited, live,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        if (auto* d = Board::GetInstance().GetDisplay()) d->UpdateStatusBar(true);
        return;
    }
    s_yield.store(false);
    EspNetwork_SetBlocked(false);
    ESP_LOGW(TAG, "radio yield end (offline=%d)", OfflinePersisted() ? 1 : 0);
    if (!OfflinePersisted()) {
        if (xTaskCreate(ApplyTask, "netmode", 6144, nullptr, 3, nullptr) != pdPASS) ESP_LOGW(TAG, "apply task create failed");
    } else if (auto* d = Board::GetInstance().GetDisplay()) {
        d->UpdateStatusBar(true);
    }
}

void SetOffline(bool on) {
    if (OfflinePersisted() == on) return;
    {
        Settings s(kNs, true);
        s.SetBool(kKey, on);
    }
    s_cached.store(on ? 1 : 0);
    // WiFi 停/起可能要等好几秒，别卡调用方（多半是 LVGL 线程）
    // 内部 RAM 栈：WiFi 停/起里可能写 NVS（关 cache），PSRAM 栈会崩
    if (xTaskCreate(ApplyTask, "netmode", 6144, on ? reinterpret_cast<void*>(1) : nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "apply task create failed");
    }
}

}  // namespace net_mode
