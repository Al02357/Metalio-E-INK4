#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 「我的 AI」历史对话：SD 卡 metalio/e-ink/my_ai_history/<id>.json + index.json。
// 纯文本，一个对话通常只有几 KB；最多保留 kMaxConversations 个，超出删最旧。
namespace my_ai {

struct HistTurn {
    std::string user;
    std::string reply;
    std::string model;
    std::string note;
    int web_refs = 0;
};

struct ConvMeta {
    std::string id;
    std::string title;
    int64_t ts = 0;   // 最后更新时间（Unix 秒；无网络对时则为 0）
    int turns = 0;
    int64_t atime = 0;       // 最后访问（打开 / 保存）时间
    bool synced = false;     // 已上传到云端
    bool cloud_only = false; // 本地文件已清理，只在云端
};

constexpr int kMaxConversations = 100;

/** @brief 新对话 id（基于当前时间） */
std::string NewConversationId();
/** @brief 保存整个对话并更新索引 */
bool SaveConversation(const std::string& id, const std::vector<HistTurn>& turns);
/** @brief 读取索引（按更新时间倒序） */
std::vector<ConvMeta> ListConversations();
/** @brief 读取一个对话 */
bool LoadConversation(const std::string& id, std::vector<HistTurn>& turns);
/** @brief 删除一个对话（本地 + 云端） */
void DeleteConversation(const std::string& id);

/** @brief 后台同步：上传未同步对话、合并云端列表、本地 7 天未访问的对话只留云端（任意线程，可重复调用） */
void CloudSyncAsync();
/** @brief 从云端取回一个只在云端的对话（阻塞，须在后台任务调用） */
bool CloudFetch(const std::string& id);
/** @brief 是否配置了云端（SD 卡 weread.json 的 server/token） */
bool CloudConfigured();
/** @brief 调试：把所有对话的访问时间往前推 sec 秒 */
void DebugAgeAll(int64_t sec);

// 界面偏好（模型、思考、联网开关），存 SD metalio/e-ink/my_ai_prefs.json
struct Prefs {
    std::string model;
    std::vector<std::pair<std::string, bool>> thinking;  // 每个模型的思考开关
    int web_mimo = -1;                                    // -1 未设置
    int web_glm = -1;
};
Prefs LoadPrefs();
void SavePrefs(const Prefs& p);

}  // namespace my_ai
