#pragma once

#include "lvgl.h"

// 「看图」：从云服务器 gallery 目录取图，4 级灰 + 抖动显示。点右半屏下一张、左半屏上一张。
class GalleryScreen {
public:
    static lv_obj_t* Create();
    static bool IsActive();
    /** 调试：n 下一张 / p 上一张 / r 重画灰度 */
    static void DebugCmd(char c);
};
