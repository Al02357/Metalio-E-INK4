#include "display/screen/standby_screen/standby_card.h"
#include "net_mode.h"
#include "feedback.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "esp_debug_cloud.h"
#include "screen_dump.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 这些设置之后会在 LVGL 任务（PSRAM 栈）里读；那里读 NVS 会关 cache 崩溃，先在这儿读进内存
    StandbyCard_Enabled();
    StandbyCard_ClockEnabled();
    net_mode::Offline();

    // 调试云 UDP 日志：默认宏关闭为空操作，不影响业务
    EspDebugCloud_Start();

    // 调试截屏：主机经 USB 串口发 "SHOT" 取当前墨水屏帧（tools/screen_shot.py）
    ScreenDump_Start();

    // 开机 1 分钟后：SD 卡上有没传的问题反馈就在后台等网上传
    xTaskCreateWithCaps(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(60000));
            if (feedback::HasPending()) feedback::UploadPendingAsync();
            vTaskDeleteWithCaps(nullptr);
        },
        "fb_boot", 4096, nullptr, 1, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
