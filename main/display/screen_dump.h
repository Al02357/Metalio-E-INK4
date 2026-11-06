#pragma once

// 调试截屏：主机经 USB Serial/JTAG 发送 "SHOT\n"，设备把当前墨水屏帧（1bpp）以 base64 文本回传。
// 另支持 "OPEN MYAI\n" 直接打开「我的 AI」页（调试用）。
// 主机侧脚本：tools/screen_shot.py。只读 RX FIFO，不安装 USJ 驱动，不影响日志输出。
#ifdef __cplusplus
extern "C" {
#endif

/** 启动后台轮询任务；可重复调用，仅首次生效 */
void ScreenDump_Start(void);

#ifdef __cplusplus
}
#endif
