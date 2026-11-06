#pragma once

#include <cstdint>
#include <vector>

// 电量记录：每 10 分钟一条写 SD metalio/e-ink/battery_log.csv（保留 7 天）。
struct BatterySample {
    int64_t t = 0;    // epoch 秒
    int level = 0;    // %
    int mv = 0;       // 电压
    int ma = 0;       // 电流（正=充电，负=放电）
    bool chg = false;
};

/** 读当前电池（电量/电压/电流），失败返回 false。 */
bool BatteryReadNow(BatterySample& out);
/** 采一条写入 SD（须在内部 RAM 栈任务里调，如 Application::Schedule）。 */
void BatteryLog_Sample();
/** 距上次记录满 10 分钟才记。in_standby=true 时由待机任务调用（不整理文件）。 */
void BatteryLog_MaybeSample(bool in_standby);
/** 读出 since（epoch 秒）之后的记录。 */
std::vector<BatterySample> BatteryLog_Read(int64_t since);
