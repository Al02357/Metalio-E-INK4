#pragma once

#include "lvgl.h"

// 「微信读书」：书架 / 进度 / 目录 / 划线走官方接口（经渲染服务转发），
// 正文由云端渲染服务提供（官方网页版逐页截图）。
// 配置见 SD 卡 metalio/e-ink/weread.json：{"server": "http://IP:端口", "token": "..."}
class WeReadScreen {
public:
    /** @brief 创建页面 */
    static lv_obj_t* Create();
    /** @brief 当前是否在本页（跨线程安全） */
    static bool IsActive();
    /** @brief 调试命令（须在 LVGL 线程），见 weread_screen.cc DebugCmd */
    static void DebugCmd(const char* cmd);
    /** @brief 离线模式关掉、网络回来了：在本页就刷新书架（顺带接着下载缓存）。任意线程 */
    static void OnNetworkBack();
};
