#include "net_mode.h"
#include "standby_card.h"
#include <esp_timer.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "ecs_power.h"
#include "board.h"
#include "display/lv_adapter_display.h"
#include "sd_paths.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "StandbyCard";
constexpr const char* kNs = "stbcard";
constexpr const char* kPath = SD_APP_ROOT "/standby_card.a2i1";
constexpr const char* kTmpPath = SD_APP_ROOT "/standby_card.tmp";
constexpr long kCardBytes = 20 + 60 * 800;  // A2I1 头 20 + 480/8×800

std::atomic<int> s_loaded{0};
std::atomic<bool> s_enabled{true};
std::atomic<bool> s_clock{true};
std::atomic<bool> s_in_use{false};
int s_last_minute = -1;

void EnsureLoaded() {
    if (s_loaded.exchange(1) != 0) return;
    Settings st(kNs, false);
    s_enabled.store(st.GetBool("on", true));
    s_clock.store(st.GetBool("clock", true));
}

void PersistTask(void*) {
    Settings st(kNs, true);
    st.SetBool("on", s_enabled.load());
    st.SetBool("clock", s_clock.load());
    vTaskDelete(nullptr);
}

void Persist() {
    // 调用方多在 LVGL 线程（栈在 PSRAM），NVS 写交给内部 RAM 栈的小任务
    xTaskCreate(PersistTask, "stbcard_nvs", 4096, nullptr, 5, nullptr);
}

bool ReadServerCfg(std::string& server, std::string& token) {
    FILE* f = std::fopen(SD_APP_ROOT "/weread.json", "rb");
    if (f == nullptr) return false;
    std::string text(2048, '\0');
    text.resize(std::fread(&text[0], 1, text.size(), f));
    std::fclose(f);
    cJSON* r = cJSON_Parse(text.c_str());
    if (r == nullptr) return false;
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

}  // namespace

bool StandbyCard_Enabled() {
    EnsureLoaded();
    return s_enabled.load();
}

void StandbyCard_SetEnabled(bool on) {
    EnsureLoaded();
    s_enabled.store(on);
    Persist();
}

bool StandbyCard_ClockEnabled() {
    EnsureLoaded();
    return s_clock.load();
}

void StandbyCard_SetClockEnabled(bool on) {
    EnsureLoaded();
    s_clock.store(on);
    Persist();
}

bool StandbyCard_TryPath(char* out, size_t out_len) {
    if (!StandbyCard_Enabled()) return false;
    struct stat st {};
    if (stat(kPath, &st) != 0 || st.st_size != kCardBytes) return false;
    if (out != nullptr && out_len > 0) strlcpy(out, kPath, out_len);
    return true;
}

static long s_card_fetched_at = 0;
long StandbyCard_FetchedAt() { return s_card_fetched_at; }

bool StandbyCard_FetchBlocking() {
    if (!StandbyCard_Enabled()) return false;
    if (net_mode::Offline() || !Board::GetInstance().IsNetworkReady()) return false;
    std::string server, token;
    if (!ReadServerCfg(server, token)) return false;
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return false;

    std::string url = server + "/api/card?fmt=a2i1";
    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        url += "&bat=" + std::to_string(level);
    }
    std::string cur, next, fw;
    if (Application::GetInstance().GetPendingUpdate(cur, next, fw)) url += "&upd=" + next;

    http->SetTimeout(60000);  // 首次当天要生成简报，可能慢
    http->SetHeader("X-Token", token);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "card open fail");
        return false;
    }
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status != 200 || static_cast<long>(body.size()) != kCardBytes || body.compare(0, 4, "A2I1") != 0) {
        ESP_LOGW(TAG, "card bad http=%d len=%u", status, static_cast<unsigned>(body.size()));
        return false;
    }
    FILE* f = std::fopen(kTmpPath, "wb");
    if (f == nullptr) return false;
    const size_t n = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    if (n != body.size()) {
        unlink(kTmpPath);
        return false;
    }
    unlink(kPath);
    s_card_fetched_at = static_cast<long>(time(nullptr));
    if (rename(kTmpPath, kPath) != 0) {
        ESP_LOGW(TAG, "card rename fail");
        return false;
    }
    ESP_LOGI(TAG, "card saved (%u bytes)", static_cast<unsigned>(body.size()));
    ecs::SyncKeyFromServer(false);  // 顺便把开关机 Key 存到 SD（已有就跳过）
    return true;
}

void StandbyCard_SetInUse(bool in_use) {
    s_in_use.store(in_use);
    if (in_use) s_last_minute = -1;
}

bool StandbyCard_ClockActive() { return s_in_use.load() && StandbyCard_ClockEnabled(); }

static char s_dbg[3072];
static size_t s_dbg_len = 0;
static portMUX_TYPE s_dbg_mux = portMUX_INITIALIZER_UNLOCKED;

void StandbyCard_DbgLog(const char* fmt, ...) {
    char line[96];
    const time_t now = time(nullptr);
    struct tm tmv {};
    localtime_r(&now, &tmv);
    int n = std::snprintf(line, sizeof(line), "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    n += std::vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n > static_cast<int>(sizeof(line)) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    portENTER_CRITICAL(&s_dbg_mux);
    if (s_dbg_len + n > sizeof(s_dbg)) {  // 满了丢掉前一半
        const size_t keep = sizeof(s_dbg) / 2;
        std::memmove(s_dbg, s_dbg + s_dbg_len - keep, keep);
        s_dbg_len = keep;
    }
    std::memcpy(s_dbg + s_dbg_len, line, n);
    s_dbg_len += n;
    portEXIT_CRITICAL(&s_dbg_mux);
}

void StandbyCard_DbgDump() {
    std::string copy;
    portENTER_CRITICAL(&s_dbg_mux);
    copy.assign(s_dbg, s_dbg_len);
    portEXIT_CRITICAL(&s_dbg_mux);
    size_t p = 0;
    while (p < copy.size()) {
        size_t e = copy.find('\n', p);
        if (e == std::string::npos) e = copy.size();
        ESP_LOGW("StbDbg", "%s", copy.substr(p, e - p).c_str());
        p = e + 1;
    }
    ESP_LOGW("StbDbg", "-- end (%u bytes)", static_cast<unsigned>(copy.size()));
}

static int64_t s_auto_exit_at = 0;
void StandbyCard_DebugAutoExit(int minutes) {
    s_auto_exit_at = minutes > 0 ? static_cast<int64_t>(time(nullptr)) + minutes * 60 : 0;
}
bool StandbyCard_DebugAutoExitDue() {
    if (s_auto_exit_at == 0 || static_cast<int64_t>(time(nullptr)) < s_auto_exit_at) return false;
    s_auto_exit_at = 0;
    return true;
}

void StandbyCard_ClockTick(bool force) {
    if (!StandbyCard_ClockActive()) {
        StandbyCard_DbgLog("tick skip: inactive in_use=%d", s_in_use.load() ? 1 : 0);
        return;
    }
    const time_t now = time(nullptr);
    struct tm tmv {};
    if (now < 1700000000 || localtime_r(&now, &tmv) == nullptr) return;
    const int minute = tmv.tm_hour * 60 + tmv.tm_min;
    if (!force && minute == s_last_minute) return;
    if (auto* d = LVAdapterDisplay::Instance()) {
        int err = 0;
        const int64_t t0 = esp_timer_get_time();
        const int rc = d->StandbyDrawClock(tmv.tm_hour, tmv.tm_min, &err);
        StandbyCard_DbgLog("clock %02d:%02d rc=%d err=0x%x %dms", tmv.tm_hour, tmv.tm_min, rc, err,
                           static_cast<int>((esp_timer_get_time() - t0) / 1000));
        if (rc == 0 || rc == 3) s_last_minute = minute;
    }
}

// 调试用「待机不浅睡」：10 分钟后自动失效，免得调试完忘了关，待机一直 50mA 耗电
static std::atomic<int64_t> s_nosleep_until{0};
void StandbyCard_DebugNoSleep(bool on) { s_nosleep_until.store(on ? esp_timer_get_time() + 10LL * 60 * 1000000 : 0); }
bool StandbyCard_DebugNoSleepOn() { return esp_timer_get_time() < s_nosleep_until.load(); }

long long StandbyCard_UsToNextMinute() {
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    const long long into = (static_cast<long long>(tv.tv_sec) % 60) * 1000000LL + tv.tv_usec;
    return 60000000LL - into + 200000LL;
}
