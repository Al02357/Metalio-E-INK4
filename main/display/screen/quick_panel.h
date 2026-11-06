#pragma once

// 下拉快捷面板：任意页面从顶部状态栏往下划打开（touch_feed 识别手势）。
// 内容：屏幕全刷、全刷频率、音量、Wi-Fi 状态、震动、电量。点面板外或按盖板键收起。
namespace QuickPanel {

/** @brief 非 LVGL 线程请求打开（touch_feed 调用） */
void RequestOpen();
/** @brief 是否已打开（跨线程） */
bool IsOpen();
/** @brief 关闭（须在 LVGL 线程） */
void Close();
/** @brief 非 LVGL 线程请求关闭 */
void RequestClose();

}  // namespace QuickPanel
