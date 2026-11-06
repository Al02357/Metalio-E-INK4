#include "ecs_power.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include "board.h"
#include "sd_paths.h"

namespace ecs {
namespace {

constexpr const char* TAG = "Ecs";
constexpr const char* kAkPath = SD_APP_ROOT "/ecs_ak.json";
void FixServerIp(const std::string& ip);

struct Ak {
    std::string id, secret, region, instance;
    bool ok() const { return !id.empty() && !secret.empty() && !region.empty() && !instance.empty(); }
};

std::string ReadFile(const char* path) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return out;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 8192) out.append(buf, n);
    std::fclose(f);
    return out;
}

std::string JStr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

Ak LoadAk() {
    Ak a;
    cJSON* r = cJSON_Parse(ReadFile(kAkPath).c_str());
    if (r == nullptr) return a;
    a.id = JStr(r, "id");
    a.secret = JStr(r, "secret");
    a.region = JStr(r, "region");
    a.instance = JStr(r, "instance");
    cJSON_Delete(r);
    return a;
}

bool ServerCfg(std::string& server, std::string& token) {
    cJSON* r = cJSON_Parse(ReadFile(SD_APP_ROOT "/weread.json").c_str());
    if (r == nullptr) return false;
    server = JStr(r, "server");
    token = JStr(r, "token");
    cJSON_Delete(r);
    while (!server.empty() && server.back() == '/') server.pop_back();
    return !server.empty() && !token.empty();
}

std::string Pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

/** 阿里云 RPC 签名 v1 调用；返回 HTTP 状态，body 写 out。 */
int Call(const Ak& ak, const char* action, std::map<std::string, std::string> p, std::string& out,
         std::string* err) {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        if (err) *err = "时间未同步";
        return -1;
    }
    // 本固件的系统时间存的是北京时间（ota.cc 把时区偏移加进了 epoch，TZ=GMT+0），签名要 UTC
    const time_t utc = now - 8 * 3600;
    struct tm g {};
    gmtime_r(&utc, &g);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g);
    char nonce[20];
    std::snprintf(nonce, sizeof(nonce), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    p["Format"] = "JSON";
    p["Version"] = "2014-05-26";
    p["AccessKeyId"] = ak.id;
    p["SignatureMethod"] = "HMAC-SHA1";
    p["Timestamp"] = ts;
    p["SignatureVersion"] = "1.0";
    p["SignatureNonce"] = nonce;
    p["Action"] = action;
    p["RegionId"] = ak.region;

    std::string canon;
    for (const auto& kv : p) {  // std::map 已按键名排序
        if (!canon.empty()) canon.push_back('&');
        canon += Pct(kv.first) + "=" + Pct(kv.second);
    }
    const std::string sts = "GET&%2F&" + Pct(canon);
    const std::string key = ak.secret + "&";
    unsigned char mac[20];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), reinterpret_cast<const unsigned char*>(key.data()),
                        key.size(), reinterpret_cast<const unsigned char*>(sts.data()), sts.size(), mac) != 0) {
        if (err) *err = "签名失败";
        return -1;
    }
    unsigned char b64[48];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, mac, sizeof(mac));
    const std::string url = "https://ecs." + ak.region + ".aliyuncs.com/?" + canon + "&Signature=" +
                            Pct(std::string(reinterpret_cast<char*>(b64), olen));

    const int64_t t_call = esp_timer_get_time();
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        if (err) *err = "无网络";
        return -1;
    }
    http->SetTimeout(15000);
    if (!http->Open("GET", url)) {
        if (err) *err = "连不上阿里云";
        return -1;
    }
    const int status = http->GetStatusCode();
    out = http->ReadAll();
    http->Close();
    if (status != 200 && err) {
        cJSON* r = cJSON_Parse(out.c_str());
        *err = r ? JStr(r, "Code") : ("HTTP " + std::to_string(status));
        if (r) cJSON_Delete(r);
    }
    ESP_LOGI(TAG, "%s -> %d (%dms)", action, status, static_cast<int>((esp_timer_get_time() - t_call) / 1000));
    return status;
}

bool ServerAlive(int timeout_ms) {
    std::string server, token;
    if (!ServerCfg(server, token)) return false;
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return false;
    http->SetTimeout(timeout_ms);
    http->SetHeader("X-Token", token);
    if (!http->Open("GET", server + "/api/sysinfo")) return false;
    const int st = http->GetStatusCode();
    http->ReadAll();
    http->Close();
    return st == 200;
}

/** 把 SD 上 weread.json / ota.json 里的 http://<旧IP>:端口 换成新 IP（节省停机再开机会换公网 IP）。 */
void FixOneFile(const char* path, const std::string& ip) {
    std::string text = ReadFile(path);
    if (text.empty()) return;
    const size_t p = text.find("http://");
    if (p == std::string::npos) return;
    const size_t h0 = p + 7;
    const size_t h1 = text.find_first_of(":/\"", h0);
    if (h1 == std::string::npos) return;
    const std::string host = text.substr(h0, h1 - h0);
    // 只改纯 IPv4 主机名
    if (host == ip || host.find_first_not_of("0123456789.") != std::string::npos) return;
    std::string out;
    size_t pos = 0;
    const std::string from = "http://" + host;
    for (size_t q; (q = text.find(from, pos)) != std::string::npos; pos = q + from.size()) {
        out.append(text, pos, q - pos);
        out += "http://" + ip;
    }
    out.append(text, pos, std::string::npos);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    ESP_LOGW(TAG, "server ip changed %s -> %s, updated %s", host.c_str(), ip.c_str(), path);
}

void FixServerIp(const std::string& ip) {
    FixOneFile(SD_APP_ROOT "/weread.json", ip);
    FixOneFile(SD_APP_ROOT "/ota.json", ip);
}

}  // namespace

bool HasKey() { return LoadAk().ok(); }

bool SyncKeyFromServer(bool force) {
    if (!force && HasKey()) return true;
    std::string server, token;
    if (!ServerCfg(server, token)) return false;
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return false;
    http->SetTimeout(10000);
    http->SetHeader("X-Token", token);
    if (!http->Open("GET", server + "/api/ecs/ak")) return false;
    const int st = http->GetStatusCode();
    const std::string body = http->ReadAll();
    http->Close();
    if (st != 200) return false;
    cJSON* r = cJSON_Parse(body.c_str());
    if (r == nullptr) return false;
    const bool ok = !JStr(r, "id").empty() && !JStr(r, "secret").empty();
    cJSON_Delete(r);
    if (!ok) return false;
    FILE* f = std::fopen(kAkPath, "wb");
    if (f == nullptr) return false;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    ESP_LOGI(TAG, "ecs key saved to SD");
    return true;
}

std::string Status(std::string* err) {
    const Ak ak = LoadAk();
    if (!ak.ok()) {
        if (err) *err = "没有开关机 Key";
        return "";
    }
    std::string body;
    if (Call(ak, "DescribeInstances", {{"InstanceIds", "[\"" + ak.instance + "\"]"}}, body, err) != 200) return "";
    cJSON* r = cJSON_Parse(body.c_str());
    std::string st;
    if (r != nullptr) {
        const cJSON* insts = cJSON_GetObjectItemCaseSensitive(r, "Instances");
        const cJSON* arr = insts ? cJSON_GetObjectItemCaseSensitive(insts, "Instance") : nullptr;
        const cJSON* first = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : nullptr;
        if (first) {
            st = JStr(first, "Status");
            // 公网 IP：EIP 优先，否则固定公网 IP（节省停机后可能会变）
            std::string ip;
            const cJSON* eip = cJSON_GetObjectItemCaseSensitive(first, "EipAddress");
            ip = eip ? JStr(eip, "IpAddress") : "";
            if (ip.empty()) {
                const cJSON* pub = cJSON_GetObjectItemCaseSensitive(first, "PublicIpAddress");
                const cJSON* arr2 = pub ? cJSON_GetObjectItemCaseSensitive(pub, "IpAddress") : nullptr;
                const cJSON* ip0 = cJSON_IsArray(arr2) ? cJSON_GetArrayItem(arr2, 0) : nullptr;
                if (cJSON_IsString(ip0)) ip = ip0->valuestring;
            }
            if (!ip.empty()) {
                ESP_LOGI(TAG, "instance %s ip=%s", st.c_str(), ip.c_str());
                FixServerIp(ip);
            }
        }
        cJSON_Delete(r);
    }
    if (st.empty() && err) *err = "查不到实例";
    return st;
}

bool Start(std::string* err) {
    const Ak ak = LoadAk();
    if (!ak.ok()) {
        if (err) *err = "没有开关机 Key";
        return false;
    }
    std::string body;
    return Call(ak, "StartInstance", {{"InstanceId", ak.instance}}, body, err) == 200;
}

bool Stop(std::string* err) {
    const Ak ak = LoadAk();
    if (!ak.ok()) {
        if (err) *err = "没有开关机 Key";
        return false;
    }
    std::string body;
    return Call(ak, "StopInstance", {{"InstanceId", ak.instance}, {"StoppedMode", "StopCharging"}}, body, err) == 200;
}

const char* StatusZh(const std::string& st) {
    if (st == "Running") return "运行中";
    if (st == "Stopped") return "已关机";
    if (st == "Starting") return "开机中";
    if (st == "Stopping") return "关机中";
    if (st.empty()) return "未知";
    return st.c_str();
}

bool WakeServerIfStopped(int wait_s, void (*progress)(const char* msg)) {
    if (!HasKey()) return false;
    std::string err;
    std::string st = Status(&err);
    ESP_LOGI(TAG, "wake check status=%s err=%s", st.c_str(), err.c_str());
    if (st == "Stopping") {
        if (progress) progress("云服务器正在关机，等它关完再开…");
        for (int i = 0; i < 24 && st == "Stopping"; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            st = Status(&err);
        }
    }
    if (st == "Stopped") {
        if (progress) progress("云服务器已关机，正在开机（约 1 分钟）…");
        if (!Start(&err)) {
            if (progress) progress(("开机失败：" + err).c_str());
            return false;
        }
    } else if (st != "Starting" && st != "Running") {
        return false;
    }
    const int64_t deadline = static_cast<int64_t>(time(nullptr)) + wait_s;
    while (static_cast<int64_t>(time(nullptr)) < deadline) {
        Status(nullptr);  // 顺便拿新公网 IP 写回配置
        if (ServerAlive(4000)) {
            ESP_LOGI(TAG, "server alive after wake");
            return true;
        }
        if (progress) progress("云服务器开机中，等服务启动…");
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
    if (progress) progress("开机超时，稍后再试");
    return false;
}

}  // namespace ecs
