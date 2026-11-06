#pragma once

/**
 * 问题反馈：用户遇到问题时长按音量下键（或串口 FEEDBACK），把当时的状态、屏幕、运行日志存到
 * SD 卡 feedback/<时间>/，联网后自动上传到自建服务器 /api/feedback，方便事后分析。
 */
namespace feedback {

/** 立即记录一份（异步，不阻塞调用方）；reason 写进 info.txt */
void Capture(const char* reason);

/** 有没上传的反馈就在后台等网上传（开机后调一次即可） */
void UploadPendingAsync();

/** SD 卡上有没上传的反馈 */
bool HasPending();

}  // namespace feedback
