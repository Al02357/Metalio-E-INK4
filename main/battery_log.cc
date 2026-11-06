#include "battery_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <esp_log.h>

#include "board.h"
#include "boards/common/power/bq27220_gauge.h"
#include "sd_paths.h"

namespace {
constexpr const char* TAG = "BatLog";
constexpr const char* kPath = SD_APP_ROOT "/battery_log.csv";
constexpr int kKeepLines = 7 * 24 * 6;  // 7 天 × 每 10 分钟
constexpr int kTrimAt = kKeepLines + 150;
}  // namespace

bool BatteryReadNow(BatterySample& out) {
    out = BatterySample{};
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(out.level, out.chg, discharging)) return false;
    auto& g = Bq27220Gauge::GetInstance();
    uint16_t mv = 0;
    int16_t ma = 0;
    if (g.GetVoltageMv(mv)) out.mv = mv;
    if (g.ReadCurrentMa(ma)) out.ma = ma;
    out.t = static_cast<int64_t>(time(nullptr));
    return true;
}

static int64_t s_last_t = 0;

static void SampleImpl(bool light) {
    BatterySample s;
    if (!BatteryReadNow(s) || s.t < 1700000000) return;
    s_last_t = s.t;
    // newlib nano 不支持 %lld：epoch 用 long（32 位够到 2038 年）
    char line[64];
    const int n = std::snprintf(line, sizeof(line), "%ld,%d,%d,%d,%d\n", static_cast<long>(s.t), s.level, s.mv, s.ma,
                                s.chg ? 1 : 0);
    FILE* f = std::fopen(kPath, "ab");
    if (f == nullptr) return;
    std::fwrite(line, 1, n, f);
    const long size = std::ftell(f);
    std::fclose(f);
    // 约 26 字节一行：超过阈值就只留最近 7 天（待机任务栈小，不在待机里整理）
    if (!light && size > kTrimAt * 26L) {
        std::vector<std::string> lines;
        if (FILE* r = std::fopen(kPath, "rb")) {
            char buf[64];
            while (std::fgets(buf, sizeof(buf), r)) lines.emplace_back(buf);
            std::fclose(r);
        }
        if (static_cast<int>(lines.size()) > kKeepLines) {
            if (FILE* w = std::fopen(SD_APP_ROOT "/battery_log.tmp", "wb")) {
                for (size_t i = lines.size() - kKeepLines; i < lines.size(); ++i) std::fputs(lines[i].c_str(), w);
                std::fclose(w);
                std::remove(kPath);
                std::rename(SD_APP_ROOT "/battery_log.tmp", kPath);
            }
        }
    }
    ESP_LOGI(TAG, "sample %d%% %dmV %dmA chg=%d%s", s.level, s.mv, s.ma, s.chg ? 1 : 0, light ? " (standby)" : "");
}

void BatteryLog_Sample() { SampleImpl(false); }

void BatteryLog_MaybeSample(bool in_standby) {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (s_last_t != 0 && now - s_last_t < 10 * 60 && now >= s_last_t) return;
    SampleImpl(in_standby);
}

std::vector<BatterySample> BatteryLog_Read(int64_t since) {
    std::vector<BatterySample> out;
    FILE* f = std::fopen(kPath, "rb");
    if (f == nullptr) return out;
    char buf[64];
    while (std::fgets(buf, sizeof(buf), f)) {
        BatterySample s;
        long t = 0;
        int lv = 0, mv = 0, ma = 0, chg = 0;
        if (std::sscanf(buf, "%ld,%d,%d,%d,%d", &t, &lv, &mv, &ma, &chg) == 5 && t >= since) {
            s.t = t;
            s.level = lv;
            s.mv = mv;
            s.ma = ma;
            s.chg = chg != 0;
            out.push_back(s);
        }
    }
    std::fclose(f);
    return out;
}
