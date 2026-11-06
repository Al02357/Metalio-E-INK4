#include <cstring>
#include "boot_key_handler.h"
#include "my_ai_screen/my_ai_screen.h"

#include <atomic>

#include <esp_log.h>
#include <lvgl.h>

#include "battery_uv_lockout.h"
#include "cloud_screen/cloud_screen.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "power_policy.h"
#include "screen_common.h"
#include "settings_screen/settings_log_upload.h"
#include "settings_screen/settings_test/settings_test_aging_screen.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

static constexpr const char* TAG = "BootKey";
static std::atomic<bool> s_boot_held{false};
static std::atomic<bool> s_long_press_fired{false};

/** 自己处理 BOOT 键、且自己占用麦克风的页面：我的AI、百问、翻译、蓝牙键鼠（按住说话） */
static bool IsAiScreen(const char* screen) {
    return screen != nullptr && (std::strcmp(screen, "my_ai") == 0 || std::strcmp(screen, "assistant") == 0 ||
                                 std::strcmp(screen, "translate") == 0 || std::strcmp(screen, "blehid") == 0);
}

static bool DispatchBootAction(BootKeyAction action, const char* kind, const char* screen) {
    if (action == nullptr) {
        ESP_LOGD(TAG, "%s no-op (screen=%s has no handler)", kind, screen);
        return false;
    }
    if (!action()) {
        ESP_LOGD(TAG, "%s ignored by screen=%s", kind, screen);
        return false;
    }
    ESP_LOGD(TAG, "%s handled by screen=%s", kind, screen);
    return true;
}

static void EnterStandbyAsync(void* /*arg*/) {
    CloudScreen::StopTransfer();
    // 已在 LVGL 异步上下文：同步拆上传层，避免 RequestAbort 再套 lv_async 与 Show 竞态踩空
    SettingsLogUpload_Teardown();
    StandbyScreen::Show();
}


void BootKey_OnPressDown() {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGD(TAG, "press-down blocked (ota upgrade)");
        return;
    }
    if (SettingsLogUpload_IsActive()) {
        ESP_LOGD(TAG, "press-down blocked (log upload)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        ESP_LOGD(TAG, "press-down blocked (aging test)");
        return;
    }
    s_boot_held.store(true, std::memory_order_release);
    s_long_press_fired.store(false, std::memory_order_release);
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGD(TAG, "press-down on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootPressDown(screen), "press-down", screen);
}

void BootKey_OnPressUp() {
    if (BatteryUvLockout_IsActive()) {
        s_boot_held.store(false, std::memory_order_release);
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        s_boot_held.store(false, std::memory_order_release);
        ESP_LOGD(TAG, "press-up blocked (ota upgrade)");
        return;
    }
    if (SettingsLogUpload_IsActive()) {
        s_boot_held.store(false, std::memory_order_release);
        ESP_LOGD(TAG, "press-up blocked (log upload)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        s_boot_held.store(false, std::memory_order_release);
        ESP_LOGD(TAG, "press-up blocked (aging test)");
        return;
    }
    s_boot_held.store(false, std::memory_order_release);
    if (MyAiScreen::OverlayBootPressUp()) return;
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGD(TAG, "press-up on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootPressUp(screen), "press-up", screen);
}

void BootKey_OnClick() {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGD(TAG, "short-press blocked (ota upgrade)");
        return;
    }
    if (SettingsLogUpload_IsActive()) {
        ESP_LOGD(TAG, "short-press blocked (log upload)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        ESP_LOGD(TAG, "short-press blocked (aging test)");
        return;
    }
    if (s_long_press_fired.load(std::memory_order_acquire)) {
        ESP_LOGD(TAG, "short-press suppressed (long-press already fired this press)");
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s", screen);
    // 我的AI 浮层打开时：短按停止 / 关闭
    if (MyAiScreen::OverlayBootClick()) return;
    // 全局：除「我的AI」「百问」和待机外，短按 = 叠出我的AI 浮层，带当前页面截图作上下文
    if (!StandbyScreen::IsActive() && !IsAiScreen(screen)) {
        MyAiScreen::RequestOverlay(false);
        return;
    }
    DispatchBootAction(VkKey_GetBootClick(screen), "short-press", screen);
}

void BootKey_OnDoubleClick() {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGD(TAG, "double-click blocked (ota upgrade)");
        return;
    }
    if (SettingsLogUpload_IsActive()) {
        ESP_LOGD(TAG, "double-click blocked (log upload)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        ESP_LOGD(TAG, "double-click blocked (aging test)");
        return;
    }
    if (s_long_press_fired.load(std::memory_order_acquire)) {
        ESP_LOGD(TAG, "double-click suppressed (long-press already fired)");
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "double-click on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootDoubleClick(screen), "double-click", screen);
}

void BootKey_OnLongPress() {
    if (BatteryUvLockout_IsActive()) {
        s_long_press_fired.store(true, std::memory_order_release);
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        s_long_press_fired.store(true, std::memory_order_release);
        ESP_LOGD(TAG, "long-press blocked (ota upgrade)");
        return;
    }
    if (SettingsLogUpload_IsActive()) {
        s_long_press_fired.store(true, std::memory_order_release);
        ESP_LOGD(TAG, "long-press blocked (log upload)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        s_long_press_fired.store(true, std::memory_order_release);
        ESP_LOGD(TAG, "long-press blocked (aging test)");
        return;
    }
    s_long_press_fired.store(true, std::memory_order_release);
    if (StandbyScreen::IsActive()) {
        if (StandbyScreen::HandleBootLongPress()) {
            ESP_LOGD(TAG, "long-press handled by standby overlay");
            return;
        }
    }
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press on screen=%s", screen);
    // 浮层打开时：长按再问一句
    if (MyAiScreen::OverlayBootLongPress()) return;
    // 全局：除「我的AI」「百问」外，长按 = 叠出我的AI 浮层并直接开始听（带当前页面截图，松手发送）
    if (!IsAiScreen(screen)) {
        MyAiScreen::RequestOverlay(true);
        return;
    }
    DispatchBootAction(VkKey_GetBootLongPress(screen), "long-press", screen);
}

void PowerKey_OnClick() {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGD(TAG, "power short blocked (ota upgrade)");
        return;
    }
    if (SettingsTestAgingScreen::IsActive()) {
        ESP_LOGD(TAG, "power short blocked (aging test)");
        return;
    }
    if (StandbyScreen::IsActive()) {
        if (PowerPolicy::GetInstance().IsLockedButAwake()) {
            // 锁屏了但后台下载还没完（没进浅睡）：电源键照样解锁
            ESP_LOGI(TAG, "power short -> unlock (bg download)");
            PowerPolicy::GetInstance().PrepareUnlockFromAwake();
            ScreenLvAsync([](void*) {
                if (StandbyScreen::IsActive()) StandbyScreen::Dismiss();
            });
            return;
        }
        // 待机 Overlay 期间 iot_button 已挂起，不应到达此处
        ESP_LOGW(TAG, "power short in standby (unexpected iot path)");
        return;
    }
    ESP_LOGI(TAG, "power short -> standby");
    PowerPolicy::GetInstance().PreparePowerKeyStandby();
    // 经 MainEventLoop 排队再无限等 LVGL 锁。勿走 ScreenLvAsyncUrgent(lock 50ms)：
    // 激活刷验证码/墨水局刷常远超 50ms，必失败并打 esp_lv_adapter ERROR；
    // 百问占主循环时 EnsureAssistantNetworkReady 间隙会 DrainScheduledTasks。
    if (!ScreenLvAsync(EnterStandbyAsync)) {
        ESP_LOGW(TAG, "power short: ScreenLvAsync failed");
    }
}

void PowerKey_OnLongPress() {
    if (BatteryUvLockout_IsActive()) {
        return;
    }
    if (OtaUpgradeScreen::IsActive() || OtaConfirmDialog::IsActive()) {
        ESP_LOGD(TAG, "power long blocked (ota upgrade)");
        return;
    }
    // 老化测试保留长按关机，便于产测救砖
    if (StandbyScreen::IsActive()) {
        ESP_LOGW(TAG, "power long in standby (unexpected iot path)");
        return;
    }
    SettingsLogUpload_RequestAbort("power_off");
    ESP_LOGI(TAG, "power long -> RequestPowerOff");
    PowerPolicy::GetInstance().RequestPowerOff();
}

bool BootKey_IsHeld() {
    return s_boot_held.load(std::memory_order_acquire);
}

bool BootKey_DidLongPress() {
    return s_long_press_fired.load(std::memory_order_acquire);
}
