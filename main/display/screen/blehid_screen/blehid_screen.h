#pragma once

#include "lvgl.h"

// 「蓝牙键鼠」：把设备当成蓝牙键盘 + 触控板 + 遥控器。进页面开蓝牙，离开页面关蓝牙。
class BleHidScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
    /** 调试：倾斜光标的轴向（swap 交换 xy，sx/sy 为 ±1） */
    static void SetTiltAxes(bool swap, int sx, int sy);
    /** 调试：按住 / 松开说话 */
    static void DebugTalk(bool on);
};
