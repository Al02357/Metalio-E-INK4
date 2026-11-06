#pragma once
#include <string>

#include <vector>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "display.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"

struct EpdFlushCtx;

struct TouchVirtualKey {
    const char* name;
    int16_t x;
    int16_t y;
    // 0=按下即触发 Click；>0=延迟到松手再触发，并在阈值到达时发 LongPress。
    uint16_t long_press_ms = 0;
};

enum class TouchVkEvent : uint8_t {
    Click = 0,       // 短按确认
    LongPress = 1,   // 达到 long_press_ms（仅配置了长按的键）
    PressUp = 2,     // 松开（仅配置了长按的键）
};

using TouchVirtualKeyEventCb = bool (*)(const char* name, TouchVkEvent event, void* user_data);

class LVAdapterDisplay : public Display {
public:
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     esp_lcd_touch_handle_t touch_handle, int width, int height);
    ~LVAdapterDisplay() override;

    // 板级构造期间 Board::GetInstance() 可能重入，屏幕创建请走本接口。
    static LVAdapterDisplay* Instance() { return instance_; }

    // 盖板虚拟键：touch_feed 独立采点并发事件，与 LVGL 刷屏并行。
    void RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count,
                                  TouchVirtualKeyEventCb cb, void* user_data = nullptr);

    void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;

    // 由各 Screen 在 Create 时绑定当前页控件；切页时覆盖。
    void BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery, lv_obj_t* status,
                           lv_obj_t* notification, lv_obj_t* low_battery_popup,
                           lv_obj_t* battery_pct = nullptr);

    /**
     * @brief 状态栏尚未绑定时排队通知；首次 BindStatusWidgets 后显示
     * @param notification 文案；nullptr/空串清除排队
     * @param duration_ms 显示时长；≤0 表示不自动隐藏
     */
    void QueueStatusNotification(const char* notification, int duration_ms = 60000);

    // 状态栏标题前缀（如「数学老师」）；非空时 SetStatus 显示为「前缀·原状态」。
    // 传 nullptr / "" 清除。须在持锁外调用（内部自带锁）。
    void SetStatusTitlePrefix(const char* prefix);

    // 全屏 A2I1 关机画：FULL（C7 + BUSY + 0x3F）。
    void ShowPoweredOffScreen();

    /** 欠压关机前全屏文案（全刷后冻结 flush） */
    void ShowLowBatteryShutdownScreen();

    // 按文件名查 resources 分区；成功时 *mem 指向 mmap，不要 free。
    bool TryGetResource(const char* name, const uint8_t** mem, size_t* size) const;

    // 进入待机前调用：先攒帧，不立即刷屏，随后在 ParkEpdForStandby 中全刷。
    void BeginStandbyEnterPaint();
    // 待机模式下排空在途刷屏，并在当前待机画面上做全刷后冻结 LVGL flush。
    void ParkEpdForStandby();
    // 退出待机后恢复 flush，并在下一帧底层画面走全刷。
    void WakeEpdFromStandby();
    /** 待机冻结期间直接在帧缓冲顶部画「HH:MM」并局刷（不经 LVGL）。成功返回 true。 */
    /** 返回 0 成功；1 未冻结/无缓冲；2 局刷失败（*err 写 esp_err）；3 无变化 */
    int StandbyDrawClock(int hh, int mm, int* err = nullptr);
    /**
     * 看图：在已上屏的黑白底（灰像素画成黑）上叠 4 级灰。
     * g2 为逻辑坐标 (x0,y0) 起 w×h 的 2bpp（高位在左；0 黑 1 深灰 2 浅灰 3 白）。
     * 须在非 LVGL 线程调用；LVGL 还没把底图刷上屏时会等（最多 timeout_ms）。返回 0 成功。
     */
    int ApplyGray4Overlay(const uint8_t* g2, int x0, int y0, int w, int h, int timeout_ms = 4000);
    /**
     * 看图：一次刷出 4 级灰（自定义完整波形：先黑白闪两下清屏，再从白往黑按灰度推不同时长）。
     * 调用前先 SetAbsorbFlush(true) 再让 LVGL 画黑白底：底图只进帧缓冲不上屏，本函数一次刷完。
     */
    int ApplyGray4Single(const uint8_t* g2, int x0, int y0, int w, int h, int timeout_ms = 4000);
    /** 打开后 LVGL 的刷屏只更新帧缓冲、不驱动屏幕（ApplyGray4Single 会关掉） */
    void SetAbsorbFlush(bool on);
    /**
     * 看图保灰：任何一次刷新都会扰动屏上的灰像素（记黑的被压黑、记白的被推白）。
     * 打开后，只落在顶部 top_h 行（状态栏）里的重画先攒着不上屏；有别的区域要重画时自动解除。
     */
    void SetGrayHold(bool on, int top_h = 0);
    /**
     * 调看图灰度波形（单位：帧）。which=0 叠加：a=深灰提亮帧数 b=浅灰提亮帧数（都为 0 = 用阅读器原表）；
     * which=1 一次刷：a=清屏每步帧数 b=浅灰 c=深灰 d=黑 加深帧数。fr=帧率字节（0 不改）。
     */
    static void SetGrayLut(int which, int a, int b, int c, int d, int fr);
    /** 刷底叠加是不是「白底往黑推」（底图里灰画成白） */
    static bool GrayOverlayFromWhite();
    /** 原始触摸回调（touch_feed 任务里调用，逻辑坐标）；period_ms 为触摸轮询周期，0 = 默认 10ms */
    /** 调试：只给原始触摸钩子喂一段直线拖动 */
    static void DebugRawDrag(int x0, int y0, int x1, int y1, int ms);
    static void SetTouchRawHook(void (*hook)(bool down, int x, int y), int period_ms);
    /** 调试：串口模拟一次点按（逻辑坐标，按住 hold_ms 毫秒） */
    static void DebugTap(int x, int y, int hold_ms);
    /** 反馈用：墨水屏刷新计数等状态，一行文本 */
    std::string DebugInfo();
    static void GetGrayLut(int which, int out[5]);

    // 调试截屏：拷贝面板原生 1bpp 帧（1=白，行优先，stride=w/8）。优先取尚未上屏的合并帧。
    bool CopyPanelFrame(std::vector<uint8_t>& out, int* w, int* h);

    // 墨水屏全刷频率：每 n 次局刷后全刷一次清残影（全刷时会黑白闪）；0 = 不自动全刷
    static int GetFullRefreshEvery();
    static void SetFullRefreshEvery(int n);  // 立即生效并写 NVS
    // 立即全刷一次（须在 LVGL 线程调用）
    void RequestFullRefreshNow();

    /** 待机 overlay 显示后：停 touch_feed（已在跑才生效；勿关全局 lv_timer，否则 lv_async dismiss 失效） */
    void PauseStandbyBackgroundWork();
    /** 退出待机 overlay：恢复 touch_feed */
    void ResumeStandbyBackgroundWork();

    // 下一帧 LVGL flush 走全刷，用于大字更新等场景，减少局刷残影。
    void RequestNextFullRefresh();

    /**
     * @brief 下一帧强制双平面局刷：0x26=旧、0x24=新（PARTIAL）
     * @note 正文沉浸进出真 4 灰 AA 时由 BookScreen 调用；开 AA 再跟灰 Overlay，关则 seed 后回 BW 局刷
     */
    void RequestDualPlanePartial();

    // 关机前调用 EPD Deep Sleep（0x10），需确保关机图已刷完。
    void SleepEpdForPowerOff();

    // CST816 硬件 RST 唤醒后，通知 esp_lv_adapter 重新读触摸（IRQ 路径）。
    void KickTouchInput();

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    void SetupUI();
    // 已持 DisplayLock（或 LVGL 任务内）；按 prefix 规则写入 status_label_，并缓存正文。
    void ApplyStatusTextLocked(const char* status);
    // 已持锁/LVGL 上下文：把缓存的网络/静音/电池/状态灌进当前绑定的控件（首帧同屏画出）。
    void RestoreStatusWidgetsLocked();

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* battery_pct_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;

    char status_title_prefix_[48] = {};
    char status_body_cache_[32] = {};  // 不含前缀的状态正文（如「待命」/「12:34」）
    char battery_pct_cache_[8] = {};   // 如「100%」
    int battery_level_cache_ = -1;     // 上次写入百分比，避免同值反复刷
    char pending_notification_[48] = {};  // Bind 前排队的状态栏通知
    int pending_notification_ms_ = 0;

    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    EpdFlushCtx* epd_flush_ctx_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;
    mmap_assets_handle_t resources_assets_ = nullptr;

    static LVAdapterDisplay* instance_;
};

// 当前 touch_feed 实况下，屏内手指是否仍按着；用于过滤迟到事件。
bool TouchUiFingerIsDown(void);

// 是否还存在未交付给 LVGL 的 down/up 边沿；翻页合并时会用到。
bool TouchUiHasPendingEdges(void);
