#pragma once

#include <string>

// 阿里云 ECS 开关机（设备直连阿里云 OpenAPI，服务器关着也能开）。
// Key：SD metalio/e-ink/ecs_ak.json {id, secret, region, instance}，由自建服务器 /api/ecs/ak 下发。
// 所有函数都会联网，阻塞，须在后台任务里调。
namespace ecs {

bool HasKey();
/** 服务器开着时从 /api/ecs/ak 取 Key 存 SD；已有就跳过（force 强制刷新）。 */
bool SyncKeyFromServer(bool force = false);
/** 查询状态：Running / Stopped / Starting / Stopping；失败返回空串并写 err。 */
std::string Status(std::string* err = nullptr);
bool Start(std::string* err = nullptr);
/** 节省停机（StopCharging）：停机后 CPU/内存不计费。 */
bool Stop(std::string* err = nullptr);
/** 状态的中文。 */
const char* StatusZh(const std::string& st);

/** 服务器连不上时调：若已停机则开机并等它的服务能访问（最多 wait_s 秒）。
 *  progress 回调用于界面提示。返回 true 表示服务已可用。 */
bool WakeServerIfStopped(int wait_s, void (*progress)(const char* msg));

}  // namespace ecs
