#include <esp_timer.h>
#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_weather_fetch.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "api_endpoints.h"
#include "sd_paths.h"
#include "standby_card.h"
#include "api_http.h"
#include "assets/lang_config.h"
#include "board.h"
#include "wifi_station.h"

void StandbyClassic_NotifyWeatherListeners() {
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] != nullptr) {
            StandbyClassic_weather_listeners[i]();
        }
    }
}

static void AsyncApplyWeatherUi(void* /*p*/) {
    StandbyClassic_ApplyWeatherUi();
    StandbyClassic_NotifyWeatherListeners();
}

void StandbyClassic_PostWeatherUi() {
    if (lv_async_call(AsyncApplyWeatherUi, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_async_call weather ui failed");
    }
}

bool StandbyClassic_ParseWeatherJson(const std::string& body, WeatherSnap& out, std::string& err) {
    out = WeatherSnap{};
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err = Lang::Strings::STANDBY_PARSE_FAIL;
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err = (cJSON_IsString(msg) && msg->valuestring != nullptr) ? msg->valuestring
                                                                   : Lang::Strings::STANDBY_WEATHER_FAIL;
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        err = Lang::Strings::STANDBY_NO_DATA;
        cJSON_Delete(root);
        return false;
    }

    cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "text");
    if (cJSON_IsString(text) && text->valuestring != nullptr) {
        strlcpy(out.text, text->valuestring, sizeof(out.text));
    }

    cJSON* lunar = cJSON_GetObjectItemCaseSensitive(data, "lunar");
    if (cJSON_IsString(lunar) && lunar->valuestring != nullptr) {
        strlcpy(out.lunar, lunar->valuestring, sizeof(out.lunar));
    }

    cJSON* city = cJSON_GetObjectItemCaseSensitive(data, "city");
    if (cJSON_IsString(city) && city->valuestring != nullptr) {
        strlcpy(out.city, city->valuestring, sizeof(out.city));
    }
    cJSON* tmin = cJSON_GetObjectItemCaseSensitive(data, "tmin");
    cJSON* tmax = cJSON_GetObjectItemCaseSensitive(data, "tmax");
    if (cJSON_IsNumber(tmin) && cJSON_IsNumber(tmax)) {
        out.tmin = tmin->valueint;
        out.tmax = tmax->valueint;
        out.has_range = true;
    }
    cJSON* tmr = cJSON_GetObjectItemCaseSensitive(data, "tomorrow");
    if (cJSON_IsString(tmr) && tmr->valuestring != nullptr) {
        strlcpy(out.tomorrow, tmr->valuestring, sizeof(out.tomorrow));
    }

    cJSON* temp = cJSON_GetObjectItemCaseSensitive(data, "temp");
    if (cJSON_IsNumber(temp)) {
        out.temp = temp->valueint;
        out.has_temp = true;
    }

    char num_buf[8] = {};
    const char* api_code = nullptr;
    cJSON* phen = cJSON_GetObjectItemCaseSensitive(data, "phenomena");
    if (phen == nullptr) {
        phen = cJSON_GetObjectItemCaseSensitive(data, "phenomenon");
    }
    if (cJSON_IsString(phen) && phen->valuestring != nullptr) {
        api_code = phen->valuestring;
    } else if (cJSON_IsNumber(phen)) {
        StandbyClassic_FormatPhenomCode(num_buf, sizeof(num_buf), phen->valueint);
        api_code = num_buf;
    }
    if (StandbyClassic_CodeKnown(api_code)) {
        strlcpy(out.icon_code, api_code, sizeof(out.icon_code));
    } else if (const char* mapped = StandbyClassic_MapTextToCode(out.text)) {
        strlcpy(out.icon_code, mapped, sizeof(out.icon_code));
    }

    out.valid = (out.text[0] != '\0') || out.has_temp;
    cJSON_Delete(root);
    if (!out.valid) {
        err = Lang::Strings::STANDBY_NO_WEATHER;
        return false;
    }
    return true;
}

namespace {
/** GET 一个 URL 拿 JSON（直连第三方，不带口令） */
cJSON* GetJson(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (http == nullptr) return nullptr;
    http->SetTimeout(15000);
    if (!http->Open("GET", url)) return nullptr;
    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "direct GET %d %s", status, url.substr(0, 60).c_str());
        return nullptr;
    }
    return cJSON_Parse(body.c_str());
}

struct WmoZh { int code; const char* text; const char* icon; };
// 同服务器 WMO_ZH：Open-Meteo 天气码 → 中文 + 原厂图标码
const WmoZh kWmo[] = {
    {0, "晴", "00"}, {1, "晴", "00"}, {2, "多云", "01"}, {3, "阴", "02"}, {45, "雾", "18"}, {48, "雾", "18"},
    {51, "小雨", "07"}, {53, "小雨", "07"}, {55, "中雨", "08"}, {56, "冻雨", "19"}, {57, "冻雨", "19"},
    {61, "小雨", "07"}, {63, "中雨", "08"}, {65, "大雨", "09"}, {66, "冻雨", "19"}, {67, "冻雨", "19"},
    {71, "小雪", "14"}, {73, "中雪", "15"}, {75, "大雪", "16"}, {77, "小雪", "14"},
    {80, "阵雨", "03"}, {81, "阵雨", "03"}, {82, "大雨", "09"}, {85, "阵雪", "13"}, {86, "阵雪", "13"},
    {95, "雷阵雨", "04"}, {96, "雷阵雨伴有冰雹", "05"}, {99, "雷阵雨伴有冰雹", "05"},
};
const WmoZh& Wmo(int c) {
    for (const auto& w : kWmo) {
        if (w.code == c) return w;
    }
    return kWmo[2];
}
int Round(double v) { return static_cast<int>(v < 0 ? v - 0.5 : v + 0.5); }
double NumAt(const cJSON* arr, int i) {
    const cJSON* v = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, i) : nullptr;
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

constexpr const char* kLocPath = SD_APP_ROOT "/wx_loc.json";

/** 自建天气成功时记下经纬度（下次服务器关着、ip-api 又连不上时用） */
void SaveLoc(const std::string& body) {
    cJSON* r = cJSON_Parse(body.c_str());
    const cJSON* d = r ? cJSON_GetObjectItemCaseSensitive(r, "data") : nullptr;
    const cJSON* lat = d ? cJSON_GetObjectItemCaseSensitive(d, "lat") : nullptr;
    const cJSON* lon = d ? cJSON_GetObjectItemCaseSensitive(d, "lon") : nullptr;
    const cJSON* city = d ? cJSON_GetObjectItemCaseSensitive(d, "city") : nullptr;
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "{\"lat\":%.4f,\"lon\":%.4f,\"city\":\"%s\"}", lat->valuedouble,
                      lon->valuedouble, cJSON_IsString(city) ? city->valuestring : "");
        if (FILE* f = std::fopen(kLocPath, "wb")) {
            std::fputs(buf, f);
            std::fclose(f);
        }
    }
    cJSON_Delete(r);
}

bool LoadLoc(double& lat, double& lon, char* city, size_t city_len) {
    FILE* f = std::fopen(kLocPath, "rb");
    if (!f) return false;
    char buf[200] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    cJSON* r = cJSON_Parse(buf);
    const cJSON* a = r ? cJSON_GetObjectItemCaseSensitive(r, "lat") : nullptr;
    const cJSON* o = r ? cJSON_GetObjectItemCaseSensitive(r, "lon") : nullptr;
    const cJSON* c = r ? cJSON_GetObjectItemCaseSensitive(r, "city") : nullptr;
    const bool ok = cJSON_IsNumber(a) && cJSON_IsNumber(o);
    if (ok) {
        lat = a->valuedouble;
        lon = o->valuedouble;
        strlcpy(city, cJSON_IsString(c) ? c->valuestring : "", city_len);
    }
    cJSON_Delete(r);
    return ok;
}

/** 云服务器关着时：ip-api 定位 + Open-Meteo，拼成和自建 /api/weather 一样的 JSON */
bool WeatherDirect(std::string& body_out) {
    static double s_lat = 0, s_lon = 0;
    static char s_city[32] = {};
    static int64_t s_loc_at = 0;
    const int64_t now = esp_timer_get_time();
    if (s_loc_at == 0 || now - s_loc_at > 6LL * 3600 * 1000000) {
        cJSON* loc = GetJson("http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon");
        if (loc != nullptr) {
            const cJSON* st = cJSON_GetObjectItemCaseSensitive(loc, "status");
            const cJSON* lat = cJSON_GetObjectItemCaseSensitive(loc, "lat");
            const cJSON* lon = cJSON_GetObjectItemCaseSensitive(loc, "lon");
            const cJSON* city = cJSON_GetObjectItemCaseSensitive(loc, "city");
            if (cJSON_IsString(st) && std::strcmp(st->valuestring, "success") == 0 && cJSON_IsNumber(lat) &&
                cJSON_IsNumber(lon)) {
                s_lat = lat->valuedouble;
                s_lon = lon->valuedouble;
                strlcpy(s_city, cJSON_IsString(city) ? city->valuestring : "", sizeof(s_city));
                // ip-api 的中文城市名带「市」，去掉
                const size_t n = std::strlen(s_city);
                if (n > 3 && std::strcmp(s_city + n - 3, "市") == 0) s_city[n - 3] = '\0';
                s_loc_at = now;
            }
            cJSON_Delete(loc);
        }
    }
    if (s_loc_at == 0) {
        // ip-api 连不上（有的网络屏蔽）：用上次自建天气记下的位置
        if (!LoadLoc(s_lat, s_lon, s_city, sizeof(s_city))) return false;
        ESP_LOGI(TAG, "weather direct: ip-api down, use saved loc %s", s_city);
    }
    char url[320];
    std::snprintf(url, sizeof(url),
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,weather_code,relative_humidity_2m"
                  "&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto&forecast_days=2",
                  s_lat, s_lon);
    cJSON* j = GetJson(url);
    if (j == nullptr) return false;
    const cJSON* cur = cJSON_GetObjectItemCaseSensitive(j, "current");
    const cJSON* d = cJSON_GetObjectItemCaseSensitive(j, "daily");
    const cJSON* t = cur ? cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m") : nullptr;
    const cJSON* wc = cur ? cJSON_GetObjectItemCaseSensitive(cur, "weather_code") : nullptr;
    bool ok = cJSON_IsNumber(t) && cJSON_IsNumber(wc) && cJSON_IsObject(d);
    if (ok) {
        const WmoZh& w = Wmo(wc->valueint);
        const cJSON* dmax = cJSON_GetObjectItemCaseSensitive(d, "temperature_2m_max");
        const cJSON* dmin = cJSON_GetObjectItemCaseSensitive(d, "temperature_2m_min");
        const cJSON* dcode = cJSON_GetObjectItemCaseSensitive(d, "weather_code");
        char tm[48];
        std::snprintf(tm, sizeof(tm), "%s %d~%d°C", Wmo(static_cast<int>(NumAt(dcode, 1))).text,
                      Round(NumAt(dmin, 1)), Round(NumAt(dmax, 1)));
        cJSON* out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "code", 0);
        cJSON* data = cJSON_AddObjectToObject(out, "data");
        cJSON_AddStringToObject(data, "text", w.text);
        cJSON_AddStringToObject(data, "phenomena", w.icon);
        cJSON_AddNumberToObject(data, "temp", Round(t->valuedouble));
        cJSON_AddNumberToObject(data, "tmin", Round(NumAt(dmin, 0)));
        cJSON_AddNumberToObject(data, "tmax", Round(NumAt(dmax, 0)));
        cJSON_AddStringToObject(data, "tomorrow", tm);
        cJSON_AddStringToObject(data, "city", s_city);
        char* txt = cJSON_PrintUnformatted(out);
        if (txt) {
            body_out = txt;
            cJSON_free(txt);
        }
        cJSON_Delete(out);
        ok = !body_out.empty();
    }
    cJSON_Delete(j);
    return ok;
}
}  // namespace

bool StandbyClassic_HttpGetWeather(std::string& body_out, std::string& err_out) {
    body_out.clear();
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = Lang::Strings::STANDBY_NO_NETWORK;
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = Lang::Strings::STANDBY_CONN_FAIL;
        return false;
    }
    // 优先用自建服务器（SD weread.json 里的 server/token）：按设备 IP 定位后查天气
    std::string url, token;
    {
        FILE* f = std::fopen(SD_APP_ROOT "/weread.json", "rb");
        if (f != nullptr) {
            std::string text(2048, '\0');
            text.resize(std::fread(&text[0], 1, text.size(), f));
            std::fclose(f);
            cJSON* r = cJSON_Parse(text.c_str());
            if (r != nullptr) {
                cJSON* sv = cJSON_GetObjectItemCaseSensitive(r, "server");
                cJSON* tk = cJSON_GetObjectItemCaseSensitive(r, "token");
                if (cJSON_IsString(sv) && cJSON_IsString(tk) && sv->valuestring[0] != '\0') {
                    url = sv->valuestring;
                    while (!url.empty() && url.back() == '/') url.pop_back();
                    url += "/api/weather";
                    token = tk->valuestring;
                }
                cJSON_Delete(r);
            }
        }
    }
    if (url.empty()) {
        url = api::WeatherLatestUrl();
    }
    if (url.empty()) {
        err_out = "weather api not configured";
        ESP_LOGI(TAG, "skip weather: cloud endpoints blank");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    if (!token.empty()) {
        http->SetHeader("X-Token", token);
    } else {
        api::ApplyCommonHeaders(http);
    }
    ESP_LOGI(TAG, "weather GET %s", token.empty() ? "vendor" : "self-hosted");
    if (!http->Open("GET", url)) {
        http->Close();
        // 自建服务器空闲关机了：设备自己按 IP 定位、直接查 Open-Meteo，不用为了天气把云服务器叫醒
        if (!token.empty() && WeatherDirect(body_out)) {
            ESP_LOGI(TAG, "weather direct ok (server down)");
            return true;
        }
        err_out = Lang::Strings::STANDBY_REQUEST_FAIL;
        api::LogHttpResponse(TAG, -1, err_out);
        return false;
    }
    const int status = http->GetStatusCode();
    body_out = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "weather http %d len=%u", status, static_cast<unsigned>(body_out.size()));
    if (!token.empty() && status == 200) SaveLoc(body_out);
    if (status < 200 || status >= 300) {
        err_out = Lang::Strings::STANDBY_REQUEST_FAIL;
        return false;
    }
    return true;
}

void StandbyClassic_EnsureWeatherCachedLocked() {
    char today[9];
    if (!StandbyClassic_FormatToday(today, sizeof(today))) {
        ESP_LOGW(TAG, "weather skip: time not ready");
        return;
    }
    if (StandbyClassic_WeatherRamIsToday()) {
        ESP_LOGD(TAG, "weather ram fresh day=%s", today);
        return;
    }
    if (StandbyClassic_busy.exchange(true)) {
        ESP_LOGD(TAG, "weather http already in flight");
        return;
    }

    WeatherSnap snap;
    bool got = false;
    if (WifiStation::GetInstance().IsConnected()) {
        std::string body;
        std::string err;
        if (!StandbyClassic_HttpGetWeather(body, err)) {
            ESP_LOGW(TAG, "weather http fail: %s", err.c_str());
        } else if (!StandbyClassic_ParseWeatherJson(body, snap, err)) {
            ESP_LOGW(TAG, "weather parse fail: %s", err.c_str());
        } else {
            snap.fetched_at = static_cast<long>(time(nullptr));
            StandbyClassic_SaveWeatherToNvs(snap, today);
            StandbyClassic_WeatherRamStore(snap, today);
            ESP_LOGI(TAG, "weather ok city=%s %s %d", snap.city, snap.text, snap.temp);
            got = true;
            StandbyCard_FetchBlocking();  // 信息卡跟天气一起刷新
        }
    }
    if (!got && !StandbyClassic_WeatherRamCopy().valid) {
        // 没网或失败：先用 NVS 里当天的旧数据顶上（fetched_at=0，下次有网会重拉）
        char nvs_day[9] = {};
        if (StandbyClassic_LoadWeatherFromNvs(snap, nvs_day, sizeof(nvs_day)) &&
            std::strcmp(nvs_day, today) == 0) {
            StandbyClassic_WeatherRamStore(snap, today);
            ESP_LOGI(TAG, "weather nvs fallback day=%s text=%s temp=%d", today, snap.text, snap.temp);
        }
    }
    StandbyClassic_busy.store(false);
}

static void EnsureWeatherTask(void* /*arg*/) {
    StandbyClassic_EnsureWeatherCachedLocked();
    StandbyClassic_fetch_task = nullptr;
    StandbyClassic_PostWeatherUi();
    vTaskDelete(nullptr);
}

void StandbyClassic_ScheduleWeatherEnsure() {
    if (StandbyClassic_WeatherRamIsToday()) {
        return;
    }
    if (StandbyClassic_fetch_task != nullptr || StandbyClassic_busy.load()) {
        return;
    }
    if (xTaskCreatePinnedToCore(EnsureWeatherTask, "standby_wx", kWorkerStack, nullptr,
                                tskIDLE_PRIORITY + 2, &StandbyClassic_fetch_task, 0) != pdPASS) {
        StandbyClassic_fetch_task = nullptr;
        ESP_LOGW(TAG, "weather task create fail");
    }
}


void StandbyClassic_EnsureWeatherCached() {
    StandbyClassic_EnsureWeatherCachedLocked();
    StandbyClassic_PostWeatherUi();
}

bool StandbyClassic_WeatherBusy() {
    return StandbyClassic_fetch_task != nullptr || StandbyClassic_busy.load();
}

void StandbyClassic_FormatWeatherLabel(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return;
    out[0] = '\0';
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    if (!snap.valid) return;
    const char* ph = StandbyClassic_LocalizedPhenomText(snap);
    // 城市 + 天气超过 7 个汉字就只显示天气，免得挤出界
    if (snap.city[0] != '\0' && std::strlen(snap.city) + std::strlen(ph) <= 21) {
        std::snprintf(out, out_len, "%s %s", snap.city, ph);
    } else {
        strlcpy(out, ph, out_len);
    }
}

size_t StandbyClassic_LocationWeatherForAi(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) return 0;
    out[0] = '\0';
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    if (!snap.valid) return 0;
    int n = 0;
    if (snap.city[0] != '\0') {
        n = std::snprintf(out, out_len, "用户所在城市：%s（按网络 IP 估算，可能不准）。", snap.city);
    }
    if (n >= 0 && static_cast<size_t>(n) < out_len) {
        n += std::snprintf(out + n, out_len - n, "当地天气：%s %d°C", StandbyClassic_LocalizedPhenomText(snap),
                           snap.temp);
    }
    if (snap.has_range && n >= 0 && static_cast<size_t>(n) < out_len) {
        n += std::snprintf(out + n, out_len - n, "，今天 %d~%d°C", snap.tmin, snap.tmax);
    }
    if (snap.tomorrow[0] != '\0' && n >= 0 && static_cast<size_t>(n) < out_len) {
        n += std::snprintf(out + n, out_len - n, "，明天 %s", snap.tomorrow);
    }
    if (n >= 0 && static_cast<size_t>(n) < out_len) n += std::snprintf(out + n, out_len - n, "。");
    return std::strlen(out);
}

bool StandbyClassic_CopyWeatherView(StandbyClassicWeatherView* out) {
    if (out == nullptr) {
        return false;
    }
    *out = StandbyClassicWeatherView{};
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    if (!snap.valid) {
        return false;
    }
    out->valid = true;
    out->has_temp = snap.has_temp;
    out->temp = snap.temp;
    StandbyClassic_FormatWeatherLabel(out->text, sizeof(out->text));
    strlcpy(out->icon_code, snap.icon_code, sizeof(out->icon_code));
    strlcpy(out->lunar, snap.lunar, sizeof(out->lunar));
    return true;
}

void StandbyClassic_FormatLunarOrWeekday(char* out, size_t out_len, const char* lunar_zh) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (std::strcmp(Lang::CODE, "zh-CN") == 0) {
        if (lunar_zh != nullptr) {
            strlcpy(out, lunar_zh, out_len);
        }
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr) {
        return;
    }
    strlcpy(out, StandbyClassic_WeekdayShortName(tm_info.tm_wday), out_len);
}

void StandbyClassic_AddWeatherUiListener(StandbyClassicWeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == cb) {
            return;
        }
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == nullptr) {
            StandbyClassic_weather_listeners[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "weather listener slots full");
}

void StandbyClassic_RemoveWeatherUiListener(StandbyClassicWeatherUiListener cb) {
    if (cb == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxWeatherListeners; ++i) {
        if (StandbyClassic_weather_listeners[i] == cb) {
            StandbyClassic_weather_listeners[i] = nullptr;
        }
    }
}

void StandbyClassic_RequestWeatherEnsure() {
    StandbyClassic_ScheduleWeatherEnsure();
}

