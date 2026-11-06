#include "feedback.h"

#include "board.h"
#include "display/lv_adapter_display.h"
#include "display/screen/vk_key_handler.h"
#include "ecs_power.h"
#include "haptic_feedback.h"
#include "net_mode.h"
#include "sd_log_writer.h"
#include "sd_paths.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_private/esp_clk.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace feedback {
namespace {

constexpr const char* TAG = "Feedback";
constexpr const char* kRoot = SD_APP_ROOT "/feedback";
constexpr size_t kLogTail = 160 * 1024;  // 最近约 160KB 日志（约十几分钟到一小时）

std::atomic<bool> s_capturing{false};
std::atomic<bool> s_uploader{false};
std::string s_reason;

bool ReadFile(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[2048];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool ReadServerCfg(std::string& server, std::string& token) {
    std::string text;
    if (!ReadFile(SD_APP_ROOT "/weread.json", text)) return false;
    cJSON* r = cJSON_Parse(text.c_str());
    if (!r) return false;
    const cJSON* sv = cJSON_GetObjectItemCaseSensitive(r, "server");
    const cJSON* tk = cJSON_GetObjectItemCaseSensitive(r, "token");
    if (cJSON_IsString(sv) && cJSON_IsString(tk)) {
        server = sv->valuestring;
        token = tk->valuestring;
    }
    cJSON_Delete(r);
    while (!server.empty() && server.back() == '/') server.pop_back();
    return !server.empty() && !token.empty();
}

const char* ResetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

std::string BuildInfo(const char* id) {
    std::string s;
    char line[256];
    auto add = [&](const char* fmt, auto... args) {
        std::snprintf(line, sizeof(line), fmt, args...);
        s += line;
        s += '\n';
    };
    add("id=%s", id);
    add("reason=%s", s_reason.c_str());
    add("version=%s", esp_app_get_description()->version);
    add("uptime_s=%lld", static_cast<long long>(esp_timer_get_time() / 1000000));
    add("reset=%s", ResetReasonName(esp_reset_reason()));
    add("cpu_mhz=%d", esp_clk_cpu_freq() / 1000000);
    add("screen=%s", VkKey_ActiveScreen() ? VkKey_ActiveScreen() : "?");
    add("heap_internal free=%u min=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    add("heap_psram free=%u min=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    int level = -1;
    bool charging = false, discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    add("battery=%d%% charging=%d discharging=%d", level, charging ? 1 : 0, discharging ? 1 : 0);
    add("offline_mode=%d network_ready=%d", net_mode::Offline() ? 1 : 0,
        Board::GetInstance().IsNetworkReady() ? 1 : 0);
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        add("wifi ssid=%s rssi=%d ch=%d", reinterpret_cast<const char*>(ap.ssid), ap.rssi, ap.primary);
    } else {
        add("wifi not connected");
    }
    if (auto* d = LVAdapterDisplay::Instance()) add("epd %s", d->DebugInfo().c_str());
    return s;
}

bool WriteFile(const std::string& path, const void* data, size_t n) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, n, f) == n;
    std::fclose(f);
    return ok;
}

/** 面板原始方向（800×480）的 PBM：P4 里 1=黑，面板帧 1=白，取反 */
bool SaveScreen(const std::string& path) {
    auto* d = LVAdapterDisplay::Instance();
    std::vector<uint8_t> fb;
    int w = 0, h = 0;
    if (!d || !d->CopyPanelFrame(fb, &w, &h) || fb.empty()) return false;
    for (auto& b : fb) b = static_cast<uint8_t>(~b);
    char hdr[32];
    const int hl = std::snprintf(hdr, sizeof(hdr), "P4\n%d %d\n", w, h);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(hdr, 1, hl, f);
    const bool ok = std::fwrite(fb.data(), 1, fb.size(), f) == fb.size();
    std::fclose(f);
    return ok;
}

bool UploadFile(const std::string& server, const std::string& token, const std::string& id,
                const std::string& dir, const char* name) {
    std::string body;
    if (!ReadFile(dir + "/" + name, body)) return true;  // 没这个文件就跳过
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return false;
    http->SetTimeout(30000);
    http->SetHeader("X-Token", token);
    http->SetHeader("Content-Type", "application/octet-stream");
    http->SetContent(std::move(body));
    const std::string url = server + "/api/feedback?id=" + id + "&name=" + name;
    if (!http->Open("POST", url)) return false;
    const int code = http->GetStatusCode();
    http->ReadAll();
    http->Close();
    if (code != 200) ESP_LOGW(TAG, "upload %s/%s -> %d", id.c_str(), name, code);
    return code == 200;
}

/** 上传所有还没传的反馈；返回是否全部成功 */
bool UploadAll() {
    std::string server, token;
    if (!ReadServerCfg(server, token)) return false;
    DIR* d = opendir(kRoot);
    if (!d) return true;
    std::vector<std::string> ids;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string dir = std::string(kRoot) + "/" + e->d_name;
        struct stat st {};
        if (stat((dir + "/uploaded").c_str(), &st) == 0) continue;
        if (stat((dir + "/info.txt").c_str(), &st) != 0) continue;
        ids.push_back(e->d_name);
    }
    closedir(d);
    bool all = true;
    for (const auto& id : ids) {
        const std::string dir = std::string(kRoot) + "/" + id;
        bool ok = true;
        for (const char* name : {"screen.pbm", "log.txt", "info.txt"}) {  // info 最后传：服务端收到 info 才附上它自己的日志
            if (!UploadFile(server, token, id, dir, name)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            WriteFile(dir + "/uploaded", "1", 1);
            ESP_LOGI(TAG, "uploaded %s", id.c_str());
        } else {
            all = false;
        }
    }
    return all;
}

void UploaderTask(void*) {
    // 最多等 6 小时有网；不主动开 WiFi（离线模式/省电时不打扰），有网了再传
    bool woke = false;
    for (int i = 0; i < 6 * 60 * 2; ++i) {
        if (!net_mode::Offline() && Board::GetInstance().IsNetworkReady()) {
            if (UploadAll()) break;
            if (!woke && ecs::HasKey()) {  // 传不上多半是云服务器关机了：开一次机
                woke = true;
                ecs::WakeServerIfStopped(180, nullptr);
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    s_uploader.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void CaptureTask(void*) {
    char id[32];
    const time_t now = time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    std::strftime(id, sizeof(id), "%Y%m%d-%H%M%S", &tm);
    mkdir(SD_APP_ROOT, 0775);
    mkdir(kRoot, 0775);
    const std::string dir = std::string(kRoot) + "/" + id;
    mkdir(dir.c_str(), 0775);

    const bool scr = SaveScreen(dir + "/screen.pbm");
    const std::string info = BuildInfo(id);
    ESP_LOGW(TAG, "capture %s (%s)\n%s", id, s_reason.c_str(), info.c_str());  // 先进日志，再导出日志
    const size_t log_n = SdLogWriter_ExportTail((dir + "/log.txt").c_str(), kLogTail);
    WriteFile(dir + "/info.txt", info.data(), info.size());
    ESP_LOGW(TAG, "saved %s screen=%d log=%uB", dir.c_str(), scr ? 1 : 0, static_cast<unsigned>(log_n));
    if (auto* d = Board::GetInstance().GetDisplay()) d->ShowNotification("已记录问题反馈", 2500);

    s_capturing.store(false);
    UploadPendingAsync();
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

void Capture(const char* reason) {
    if (s_capturing.exchange(true)) return;
    s_reason = reason ? reason : "";
    HapticPulseIfEnabled();
    if (xTaskCreateWithCaps(CaptureTask, "feedback", 8192, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_capturing.store(false);
        ESP_LOGE(TAG, "task create failed");
    }
}

bool HasPending() {
    DIR* d = opendir(kRoot);
    if (!d) return false;
    bool any = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        struct stat st {};
        if (stat((std::string(kRoot) + "/" + e->d_name + "/uploaded").c_str(), &st) != 0) {
            any = true;
            break;
        }
    }
    closedir(d);
    return any;
}

void UploadPendingAsync() {
    if (s_uploader.exchange(true)) return;
    if (xTaskCreateWithCaps(UploaderTask, "fb_upload", 8192, nullptr, 2, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_uploader.store(false);
    }
}

}  // namespace feedback
