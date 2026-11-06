#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

// 低功耗蓝牙 HID（键盘 + 鼠标 + 媒体键）。只在「蓝牙键鼠」页打开时运行，离开页面整个蓝牙栈关掉。
namespace ble_hid {

enum class State : uint8_t { kOff, kStarting, kAdvertising, kConnected };

/** 打开蓝牙并开始广播（已开则什么也不做）。阻塞到协议栈起来，约 0.3 秒 */
bool Start();
/** 断开、停广播、关掉整个蓝牙栈，释放内存 */
void Stop();
State GetState();
/** 配对时要在电脑上输入的 6 位数字；不在配对中返回 -1 */
int PendingPasskey();
/** 最近一次加密失败（多半是主机还留着旧配对，要在主机上删掉重配） */
bool AuthFailed();
/** 清掉所有已配对的设备（下次要重新配对） */
void ForgetPeers();
/** 状态变化回调（在蓝牙任务里调用，不要在里面碰 LVGL） */
void SetStateCallback(void (*cb)());

// ---- 发送（未连接时直接丢弃） ----
/** 鼠标：相对移动 + 滚轮，buttons 位 0 左 1 右 2 中 */
void Mouse(int dx, int dy, int wheel, uint8_t buttons);
/** 鼠标按键状态（拖动用：按下后保持，再调一次松开） */
void MouseButtons(uint8_t buttons);
uint8_t MouseButtonsHeld();
/** 按一下键（按下再松开）；mod 为 HID 修饰键位（1 Ctrl 2 Shift 4 Alt 8 Win） */
void KeyTap(uint8_t mod, uint8_t usage);
/** 输入 ASCII 文本（美式键盘布局） */
void TypeAscii(const std::string& s);
/** ASCII 字符对应的 HID 用法码（美式布局）；不支持返回 false */
bool AsciiToUsage(char c, uint8_t& usage, bool& shift);
/** 模拟手指滑动（触摸屏报告，坐标 0..10000 对应主机整块屏），steps 段，每段约 10ms */
void Swipe(int x0, int y0, int x1, int y1, int steps = 12);
/** 模拟手指点一下 */
void Tap(int x, int y);
// ---- 语音（按住说话：麦克风 16kHz → IMA ADPCM → 自定义 GATT 通知，电脑端程序接收） ----
/** 电脑端程序已连上并订阅了音频 */
bool VoiceHostReady();
/** engine：识别方式（0 电脑本机 1 自己服务器 2 MiMo），由电脑端程序照着用 */
void VoiceBegin(uint8_t engine = 0);
/** 电脑端回写的识别结果（在蓝牙任务里调用，别碰 LVGL）：status 0 成功 1 丢弃 2 失败 */
void SetVoiceResultCallback(void (*cb)(int status, const char* text));
/** 喂 16kHz 单声道 PCM（可在音频任务里调用） */
void VoicePcm(const int16_t* pcm, size_t n);
void VoiceEnd();
/** 媒体键（Consumer Usage，如 0xE9 音量+） */
void Consumer(uint16_t usage);

}  // namespace ble_hid
