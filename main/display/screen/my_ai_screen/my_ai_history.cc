#include "my_ai_history.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "sd_paths.h"
#include "board.h"
#include "power_policy.h"
#include <atomic>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace my_ai {
namespace {

constexpr const char* TAG = "MyAiHistory";
constexpr const char* kDir = SD_APP_ROOT "/my_ai_history";
constexpr const char* kIndex = SD_APP_ROOT "/my_ai_history/index.json";
constexpr const char* kPrefs = SD_APP_ROOT "/my_ai_prefs.json";
constexpr long kMaxFileBytes = 512 * 1024;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::atomic<bool> s_sync_kick{false};
std::atomic<bool> s_sync_running{false};
constexpr const char* kTomb = SD_APP_ROOT "/my_ai_history/deleted.txt";  // 待从云端删除的 id，一行一个
constexpr int64_t kLocalKeepSec = 7 * 24 * 3600;

void AddTombstoneLocked(const std::string& id) {
    if (FILE* f = std::fopen(kTomb, "ab")) {
        std::fprintf(f, "%s\n", id.c_str());
        std::fclose(f);
    }
}

std::mutex& Mu() {
    static std::mutex mu;
    return mu;
}

bool EnsureDir() {
    struct stat st;
    if (stat(kDir, &st) == 0) return true;
    return mkdir(kDir, 0775) == 0;
}

std::string PathOf(const std::string& id) { return std::string(kDir) + "/" + id + ".json"; }

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > kMaxFileBytes) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(n);
    return n > 0;
}

// 先写临时文件再改名，掉电时不会留下半个 JSON
bool WriteFileAtomic(const std::string& path, const char* data) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t len = std::strlen(data);
    const bool ok = std::fwrite(data, 1, len, f) == len;
    std::fclose(f);
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    unlink(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

int64_t NowSec() {
    const time_t t = time(nullptr);
    return t > 1700000000 ? static_cast<int64_t>(t) : 0;
}

std::string Str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
}

int64_t Num(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? static_cast<int64_t>(v->valuedouble) : 0;
}

std::vector<ConvMeta> ReadIndexLocked() {
    std::vector<ConvMeta> out;
    std::string text;
    if (!ReadFile(kIndex, text)) return out;
    JsonPtr root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root || !cJSON_IsArray(root.get())) return out;
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, root.get()) {
        ConvMeta m;
        m.id = Str(it, "id");
        m.title = Str(it, "t");
        m.ts = Num(it, "ts");
        m.turns = static_cast<int>(Num(it, "n"));
        m.atime = Num(it, "a");
        m.synced = Num(it, "s") != 0;
        m.cloud_only = Num(it, "c") != 0;
        if (m.atime == 0) m.atime = m.ts;
        if (!m.id.empty()) out.push_back(std::move(m));
    }
    return out;
}

bool WriteIndexLocked(const std::vector<ConvMeta>& list) {
    JsonPtr root(cJSON_CreateArray(), cJSON_Delete);
    for (const auto& m : list) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m.id.c_str());
        cJSON_AddStringToObject(o, "t", m.title.c_str());
        cJSON_AddNumberToObject(o, "ts", static_cast<double>(m.ts));
        cJSON_AddNumberToObject(o, "n", m.turns);
        cJSON_AddNumberToObject(o, "a", static_cast<double>(m.atime));
        if (m.synced) cJSON_AddNumberToObject(o, "s", 1);
        if (m.cloud_only) cJSON_AddNumberToObject(o, "c", 1);
        cJSON_AddItemToArray(root.get(), o);
    }
    char* raw = cJSON_PrintUnformatted(root.get());
    if (raw == nullptr) return false;
    const bool ok = WriteFileAtomic(kIndex, raw);
    cJSON_free(raw);
    return ok;
}

// 标题：第一句提问，最多约 40 字节，按 UTF-8 边界截断
std::string MakeTitle(const std::vector<HistTurn>& turns) {
    for (const auto& t : turns) {
        if (t.user.empty()) continue;
        std::string s = t.user;
        for (char& c : s) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        if (s.size() > 60) {
            size_t cut = 60;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
            s = s.substr(0, cut) + "…";
        }
        return s;
    }
    return "（空对话）";
}

}  // namespace

std::string NewConversationId() {
    char buf[40];
    const time_t t = time(nullptr);
    struct tm tmv;
    if (t > 1700000000 && localtime_r(&t, &tmv) != nullptr) {
        std::snprintf(buf, sizeof(buf), "c%04d%02d%02d_%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                      tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
        std::snprintf(buf, sizeof(buf), "c_boot%lld", static_cast<long long>(esp_timer_get_time() / 1000));
    }
    return buf;
}

bool SaveConversation(const std::string& id, const std::vector<HistTurn>& turns) {
    if (id.empty() || turns.empty()) return false;
    std::lock_guard<std::mutex> lock(Mu());
    if (!EnsureDir()) {
        ESP_LOGW(TAG, "mkdir %s failed (no SD?)", kDir);
        return false;
    }
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "id", id.c_str());
    cJSON_AddNumberToObject(root.get(), "ts", static_cast<double>(NowSec()));
    cJSON* arr = cJSON_AddArrayToObject(root.get(), "turns");
    for (const auto& t : turns) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "u", t.user.c_str());
        cJSON_AddStringToObject(o, "a", t.reply.c_str());
        cJSON_AddStringToObject(o, "m", t.model.c_str());
        if (!t.note.empty()) cJSON_AddStringToObject(o, "e", t.note.c_str());
        if (t.web_refs > 0) cJSON_AddNumberToObject(o, "w", t.web_refs);
        cJSON_AddItemToArray(arr, o);
    }
    char* raw = cJSON_PrintUnformatted(root.get());
    if (raw == nullptr) return false;
    const bool ok = WriteFileAtomic(PathOf(id), raw);
    cJSON_free(raw);
    if (!ok) {
        ESP_LOGW(TAG, "write %s failed", id.c_str());
        return false;
    }

    auto list = ReadIndexLocked();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const ConvMeta& m) { return m.id == id; }),
               list.end());
    ConvMeta m;
    m.id = id;
    m.title = MakeTitle(turns);
    m.ts = NowSec();
    m.atime = m.ts;
    m.turns = static_cast<int>(turns.size());
    list.insert(list.begin(), std::move(m));
    while (static_cast<int>(list.size()) > kMaxConversations) {
        unlink(PathOf(list.back().id).c_str());
        list.pop_back();
    }
    WriteIndexLocked(list);
    // 保存后马上在后台上传（锁外调用）
    ESP_LOGI(TAG, "saved %s (%u turns), %u conversations", id.c_str(), static_cast<unsigned>(turns.size()),
             static_cast<unsigned>(list.size()));
    s_sync_kick.store(true);
    return true;
}

std::vector<ConvMeta> ListConversations() {
    std::lock_guard<std::mutex> lock(Mu());
    return ReadIndexLocked();
}

bool LoadConversation(const std::string& id, std::vector<HistTurn>& turns) {
    std::lock_guard<std::mutex> lock(Mu());
    std::string text;
    if (!ReadFile(PathOf(id), text)) return false;
    JsonPtr root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root) return false;
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root.get(), "turns");
    if (!cJSON_IsArray(arr)) return false;
    turns.clear();
    const cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        HistTurn t;
        t.user = Str(it, "u");
        t.reply = Str(it, "a");
        t.model = Str(it, "m");
        t.note = Str(it, "e");
        t.web_refs = static_cast<int>(Num(it, "w"));
        turns.push_back(std::move(t));
    }
    // 记访问时间（7 天未访问才清理本地）
    auto list = ReadIndexLocked();
    for (auto& m : list) {
        if (m.id == id && NowSec() > 0) {
            m.atime = NowSec();
            WriteIndexLocked(list);
            break;
        }
    }
    return true;
}

void DeleteConversation(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(Mu());
        unlink(PathOf(id).c_str());
        auto list = ReadIndexLocked();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const ConvMeta& m) { return m.id == id; }),
                   list.end());
        WriteIndexLocked(list);
        AddTombstoneLocked(id);
    }
    CloudSyncAsync();
}

Prefs LoadPrefs() {
    Prefs p;
    std::lock_guard<std::mutex> lock(Mu());
    std::string text;
    if (!ReadFile(kPrefs, text)) return p;
    JsonPtr root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root) return p;
    p.model = Str(root.get(), "model");
    const cJSON* ws = cJSON_GetObjectItemCaseSensitive(root.get(), "web_search");  // 旧版
    if (cJSON_IsBool(ws)) p.web_mimo = cJSON_IsTrue(ws) ? 1 : 0;
    ws = cJSON_GetObjectItemCaseSensitive(root.get(), "web_mimo");
    if (cJSON_IsBool(ws)) p.web_mimo = cJSON_IsTrue(ws) ? 1 : 0;
    ws = cJSON_GetObjectItemCaseSensitive(root.get(), "web_glm");
    if (cJSON_IsBool(ws)) p.web_glm = cJSON_IsTrue(ws) ? 1 : 0;
    const cJSON* th = cJSON_GetObjectItemCaseSensitive(root.get(), "thinking");
    if (cJSON_IsObject(th)) {
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, th) {
            if (cJSON_IsBool(it) && it->string) p.thinking.emplace_back(it->string, cJSON_IsTrue(it));
        }
    }
    return p;
}

void SavePrefs(const Prefs& p) {
    std::lock_guard<std::mutex> lock(Mu());
    JsonPtr root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "model", p.model.c_str());
    if (p.web_mimo >= 0) cJSON_AddBoolToObject(root.get(), "web_mimo", p.web_mimo == 1);
    if (p.web_glm >= 0) cJSON_AddBoolToObject(root.get(), "web_glm", p.web_glm == 1);
    cJSON* th = cJSON_AddObjectToObject(root.get(), "thinking");
    for (const auto& kv : p.thinking) cJSON_AddBoolToObject(th, kv.first.c_str(), kv.second);
    char* raw = cJSON_PrintUnformatted(root.get());
    if (raw == nullptr) return;
    WriteFileAtomic(kPrefs, raw);
    cJSON_free(raw);
}

// ---------------------------------------------------------------- 云端同步

namespace {

struct CloudCfg {
    std::string server, token;
};

CloudCfg LoadCloudCfg() {
    CloudCfg c;
    std::string text;
    if (!ReadFile(SD_APP_ROOT "/weread.json", text)) return c;
    JsonPtr root(cJSON_Parse(text.c_str()), cJSON_Delete);
    if (!root) return c;
    c.server = Str(root.get(), "server");
    c.token = Str(root.get(), "token");
    while (!c.server.empty() && c.server.back() == '/') c.server.pop_back();
    return c;
}

bool CloudReq(const CloudCfg& cfg, const char* method, const std::string& path, std::string body,
              std::string* out) {
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) return false;
    http->SetTimeout(20000);
    http->SetHeader("X-Token", cfg.token);
    if (std::strcmp(method, "POST") == 0) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (!http->Open(method, cfg.server + path)) return false;
    const int status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGW(TAG, "cloud %s %s -> %d", method, path.c_str(), status);
        return false;
    }
    if (out) *out = std::move(resp);
    return true;
}

void SyncOnce(const CloudCfg& cfg) {
    // 1) 云端删除
    std::string tomb;
    {
        std::lock_guard<std::mutex> lock(Mu());
        ReadFile(kTomb, tomb);
    }
    if (!tomb.empty()) {
        bool all_ok = true;
        size_t p = 0;
        while (p < tomb.size()) {
            size_t e = tomb.find('\n', p);
            if (e == std::string::npos) e = tomb.size();
            const std::string id = tomb.substr(p, e - p);
            if (!id.empty()) all_ok &= CloudReq(cfg, "POST", "/api/ai/conv/delete?id=" + id, "", nullptr);
            p = e + 1;
        }
        if (all_ok) {
            std::lock_guard<std::mutex> lock(Mu());
            unlink(kTomb);
        }
    }
    // 2) 上传未同步的
    std::vector<ConvMeta> list;
    {
        std::lock_guard<std::mutex> lock(Mu());
        list = ReadIndexLocked();
    }
    int up = 0;
    for (const auto& m : list) {
        if (m.synced || m.cloud_only) continue;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(Mu());
            if (!ReadFile(PathOf(m.id), text)) continue;
        }
        if (!CloudReq(cfg, "POST", "/api/ai/conv?id=" + m.id, std::move(text), nullptr)) break;
        std::lock_guard<std::mutex> lock(Mu());
        auto cur = ReadIndexLocked();
        for (auto& c : cur) {
            if (c.id == m.id && c.ts == m.ts) c.synced = true;  // 上传期间又改过的不算
        }
        WriteIndexLocked(cur);
        ++up;
    }
    // 3) 合并云端列表（换卡 / 误删后可找回）
    std::string resp;
    int merged = 0;
    if (CloudReq(cfg, "GET", "/api/ai/convs", "", &resp)) {
        JsonPtr root(cJSON_Parse(resp.c_str()), cJSON_Delete);
        const cJSON* arr = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "convs") : nullptr;
        std::lock_guard<std::mutex> lock(Mu());
        auto cur = ReadIndexLocked();
        const cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            const std::string id = Str(it, "id");
            if (id.empty()) continue;
            bool have = false;
            for (const auto& c : cur) have |= (c.id == id);
            if (have || tomb.find(id) != std::string::npos) continue;
            ConvMeta m;
            m.id = id;
            m.title = Str(it, "title");
            m.ts = Num(it, "ts");
            m.atime = m.ts;
            m.turns = static_cast<int>(Num(it, "turns"));
            m.synced = true;
            m.cloud_only = true;
            cur.push_back(std::move(m));
            ++merged;
        }
        if (merged) {
            std::sort(cur.begin(), cur.end(), [](const ConvMeta& a, const ConvMeta& b) { return a.ts > b.ts; });
            WriteIndexLocked(cur);
        }
    }
    // 4) 本地 7 天未访问且已上传的：删本地文件，只留云端
    int pruned = 0;
    const int64_t now = NowSec();
    if (now > 0) {
        std::lock_guard<std::mutex> lock(Mu());
        auto cur = ReadIndexLocked();
        for (auto& c : cur) {
            if (c.synced && !c.cloud_only && c.atime > 0 && now - c.atime > kLocalKeepSec) {
                unlink(PathOf(c.id).c_str());
                c.cloud_only = true;
                ++pruned;
            }
        }
        if (pruned) WriteIndexLocked(cur);
    }
    ESP_LOGI(TAG, "cloud sync: uploaded %d, merged %d, pruned %d", up, merged, pruned);
}

void SyncTask(void*) {
    const CloudCfg cfg = LoadCloudCfg();
    do {
        s_sync_kick.store(false);
        PowerNeedHold hold(PowerNeed::OtaDownload);
        auto& board = Board::GetInstance();
        if (!board.IsNetworkReady() && !board.EnsureNetworkReady(20000)) break;
        SyncOnce(cfg);
    } while (s_sync_kick.load());
    s_sync_running.store(false);
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

void DebugAgeAll(int64_t sec) {
    std::lock_guard<std::mutex> lock(Mu());
    auto list = ReadIndexLocked();
    for (auto& m : list) m.atime = (m.atime > sec ? m.atime : NowSec()) - sec;
    WriteIndexLocked(list);
}

bool CloudConfigured() {
    const CloudCfg c = LoadCloudCfg();
    return !c.server.empty() && !c.token.empty();
}

void CloudSyncAsync() {
    s_sync_kick.store(true);
    if (!CloudConfigured() || s_sync_running.exchange(true)) return;
    if (xTaskCreateWithCaps(SyncTask, "my_ai_sync", 8 * 1024, nullptr, 2, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_sync_running.store(false);
    }
}

bool CloudFetch(const std::string& id) {
    const CloudCfg cfg = LoadCloudCfg();
    if (cfg.server.empty()) return false;
    std::string text;
    {
        PowerNeedHold hold(PowerNeed::OtaDownload);
        auto& board = Board::GetInstance();
        if (!board.IsNetworkReady() && !board.EnsureNetworkReady(20000)) return false;
        if (!CloudReq(cfg, "GET", "/api/ai/conv?id=" + id, "", &text)) return false;
    }
    std::lock_guard<std::mutex> lock(Mu());
    if (!EnsureDir() || !WriteFileAtomic(PathOf(id), text.c_str())) return false;
    auto list = ReadIndexLocked();
    for (auto& m : list) {
        if (m.id == id) {
            m.cloud_only = false;
            m.synced = true;
            m.atime = NowSec() > 0 ? NowSec() : m.atime;
        }
    }
    WriteIndexLocked(list);
    ESP_LOGI(TAG, "fetched %s from cloud (%u B)", id.c_str(), static_cast<unsigned>(text.size()));
    return true;
}

}  // namespace my_ai
