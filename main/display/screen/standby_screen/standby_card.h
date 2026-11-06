#pragma once
#include "lvgl.h"

#include <cstddef>

// 待机信息卡：自建服务器 /api/card 生成的 480×800 A2I1，存 SD。
// 待机：没设待机壁纸时显示信息卡；关机：没设关机壁纸时用信息卡（顶部叠「已关机」提示）。
// 时钟：信息卡顶部 0..120 留白，待机冻结后由设备每分钟局刷「HH:MM」。

bool StandbyCard_Enabled();
void StandbyCard_SetEnabled(bool on);
bool StandbyCard_ClockEnabled();
void StandbyCard_SetClockEnabled(bool on);

/** 信息卡开启且文件有效时写出路径并返回 true。 */
bool StandbyCard_TryPath(char* out, size_t out_len);

/** 联网拉一张新卡写入 SD（阻塞，须在内部 RAM 栈任务里调）。 */
bool StandbyCard_FetchBlocking();

/** 待机页当前是否在显示信息卡（由 StandbyScreen 挂载/卸载时设置）。 */
void StandbyCard_SetInUse(bool in_use);

/**
 * 信息卡上盖一层本地内容（LVGL 线程、建好壁纸图之后调）：天气用设备自己拉的（不靠云服务器开机），
 * 电量用当前读数。天气数据还没有时保留卡上原样。
 */
void StandbyCard_BuildLocal(lv_obj_t* root);

/** 这次开机里最近一次下载到信息卡的时刻（time()），0 = 还没下载过 */
long StandbyCard_FetchedAt();
bool StandbyCard_ClockActive();

/** 待机 LP 循环里调：分钟变了就局刷时钟；force=true 无条件画一次。 */
void StandbyCard_ClockTick(bool force);
/** 距离下一个整分钟的微秒数（+200ms 余量）。 */
long long StandbyCard_UsToNextMinute();
/** 调试：待机不进浅睡（串口不断，便于截图）。 */
void StandbyCard_DebugNoSleep(bool on);
bool StandbyCard_DebugNoSleepOn();
/** 待机调试环形日志（RAM，浅睡期间串口断开时用）；出待机后用串口 STBDBG 打印。 */
void StandbyCard_DbgLog(const char* fmt, ...);
void StandbyCard_DbgDump();
/** 调试：待机 n 分钟后自动退出（0 关闭）。 */
void StandbyCard_DebugAutoExit(int minutes);
bool StandbyCard_DebugAutoExitDue();
