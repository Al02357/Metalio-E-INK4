#pragma once

/**
 * 离线模式（快捷面板开关）：WiFi 完全关闭、不搜网；联网业务立即失败（微信读书直接读缓存）。
 * 存 NVS，重启后保持。
 */
namespace net_mode {

bool Offline();

/** 切换；关→开会暂停 WiFi，开→关会在后台拉起 WiFi。可在任意线程调用。 */
void SetOffline(bool on);

/**
 * 临时让出射频（蓝牙键鼠页用）：WiFi 整个关掉并释放内存，期间 Offline() 也返回 true。
 * on=true 阻塞到 WiFi 关完；on=false 后台按原来的离线设置决定要不要重新联网。不写 NVS。
 * 要在内部 RAM 栈的任务里调用（关 WiFi 可能写 NVS）。
 */
void SetRadioYield(bool on);
bool RadioYielded();

}  // namespace net_mode
