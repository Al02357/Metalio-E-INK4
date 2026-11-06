#pragma once

#include "lvgl.h"

// 「监控」：本机电量曲线 / 电压电流 / 内存 / SD / Wi-Fi，云服务器状态 + 开关机 + 自动关机。
class MonitorScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
    /** 调试：r 刷新 / 1 24小时 / 7 七天 */
    static void DebugCmd(char c);
};
