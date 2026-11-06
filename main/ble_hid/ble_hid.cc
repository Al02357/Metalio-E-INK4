#include "ble_hid.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_hidd.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

extern "C" void ble_store_config_init(void);
extern "C" int ble_gatts_conn_can_alloc(void);  // NimBLE 内部函数，见 Stop()

namespace ble_hid {
namespace {

constexpr const char* TAG = "BleHid";
constexpr const char* kName = "Metalio E-Ink";
constexpr uint8_t kRidKbd = 1, kRidMouse = 2, kRidCc = 3, kRidTouch = 4;

// 键盘(1) + 鼠标(2) + 媒体键(3)
const uint8_t kReportMap[] = {
    // 键盘
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, kRidKbd,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  // 修饰键
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                              // 保留
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,                          // 指示灯
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,  // 6 个键
    0xC0,
    // 鼠标
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, kRidMouse, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
    // 媒体键
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, kRidCc,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
    // 单点触摸屏（手机上模拟手指点、滑：抖音上下滑、阅读 App 点左右边）。坐标 0..10000 对应整块屏。
    // 故意不带「触点编号 / 触点数」：带了 Linux / 安卓会用 hid-multitouch 驱动接管整个设备，把鼠标那一路丢掉
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, kRidTouch,
    0x09, 0x22, 0xA1, 0x00,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,  // 按下
    0x09, 0x32, 0x81, 0x02,                                                  // 在范围内
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x15, 0x00, 0x26, 0x10, 0x27, 0x75, 0x10, 0x95, 0x01,
    0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,                          // X / Y
    0xC0,
    0xC0,
};

esp_hid_raw_report_map_t s_maps[] = {{kReportMap, sizeof(kReportMap)}};
esp_hid_device_config_t s_cfg = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = kName,
    .manufacturer_name = "Metalio",
    .serial_number = "0001",
    .report_maps = s_maps,
    .report_maps_len = 1,
};

std::mutex s_api_mu;  // Start/Stop
std::atomic<State> s_state{State::kOff};
std::atomic<int> s_passkey{-1};
std::atomic<uint16_t> s_conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> s_encrypted{false};
std::atomic<bool> s_auth_fail{false};  // 主机拿旧配对信息来连、对不上
void (*s_cb)() = nullptr;
esp_hidd_dev_t* s_dev = nullptr;
SemaphoreHandle_t s_synced = nullptr;
bool s_mem_released = false;

// 发送：鼠标位移在这里攒着，由发送任务按 10ms 节拍合并发出；按键走队列保证顺序
std::atomic<int> s_mdx{0}, s_mdy{0}, s_mwheel{0};
std::atomic<uint8_t> s_mbtn{0};
std::atomic<bool> s_mbtn_dirty{false};
struct Rpt {
    uint8_t id, len, data[8];
};
QueueHandle_t s_q = nullptr;
TaskHandle_t s_tx_task = nullptr;
std::atomic<bool> s_tx_run{false};

// ---------------------------------------------------------------- 语音（自定义 GATT 服务）
// 服务 e1c80001-6f3a-4b2e-9d7a-3f5b2c1a0e4d
//   音频 e1c80002（通知）：[seq u16][预测值 i16][步长索引 u8][保留 u8] + IMA ADPCM（16kHz 单声道，低半字节在前）
//   控制 e1c80003（读 / 通知 / 写）：通知 0x01 开始(+采样率 u16) / 0x02 结束；读出版本串
const ble_uuid128_t kVoiceSvcUuid = {{BLE_UUID_TYPE_128},
                                     {0x4d, 0x0e, 0x1a, 0x2c, 0x5b, 0x3f, 0x7a, 0x9d, 0x2e, 0x4b, 0x3a, 0x6f, 0x01, 0x00, 0xc8, 0xe1}};
const ble_uuid128_t kVoiceAudioUuid = {{BLE_UUID_TYPE_128},
                                       {0x4d, 0x0e, 0x1a, 0x2c, 0x5b, 0x3f, 0x7a, 0x9d, 0x2e, 0x4b, 0x3a, 0x6f, 0x02, 0x00, 0xc8, 0xe1}};
const ble_uuid128_t kVoiceCtrlUuid = {{BLE_UUID_TYPE_128},
                                      {0x4d, 0x0e, 0x1a, 0x2c, 0x5b, 0x3f, 0x7a, 0x9d, 0x2e, 0x4b, 0x3a, 0x6f, 0x03, 0x00, 0xc8, 0xe1}};
constexpr const char* kVoiceVersion = "EINK-VOICE 1 16000 ima-adpcm";
uint16_t s_audio_h = 0, s_ctrl_h = 0;
std::atomic<bool> s_audio_sub{false};

void (*s_vresult_cb)(int status, const char* text) = nullptr;

int VoiceAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, kVoiceVersion, std::strlen(kVoiceVersion)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // 电脑端回写识别结果：[0x10][状态 0 成功 / 1 丢弃 / 2 失败][UTF-8 文字]
        char buf[256];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &n) != 0 && n == 0) return 0;
        buf[n] = 0;
        if (n >= 2 && static_cast<uint8_t>(buf[0]) == 0x10 && s_vresult_cb) s_vresult_cb(buf[1], buf + 2);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

const ble_gatt_chr_def kVoiceChrs[] = {
    {.uuid = &kVoiceAudioUuid.u, .access_cb = VoiceAccess, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_audio_h},
    {.uuid = &kVoiceCtrlUuid.u,
     .access_cb = VoiceAccess,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
     .val_handle = &s_ctrl_h},
    {},
};
const ble_gatt_svc_def kVoiceSvcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kVoiceSvcUuid.u, .characteristics = kVoiceChrs},
    {},
};

// 语音包队列（PSRAM）：控制包和音频包按顺序发
struct VPkt {
    uint8_t type;  // 0 音频 1 控制
    uint8_t len;
    uint8_t data[200];
};
QueueHandle_t s_vq = nullptr;
std::atomic<bool> s_voice_on{false};
std::mutex s_vmu;
int16_t s_vbuf[320];
int s_vn = 0;
uint16_t s_vseq = 0;
int s_apred = 0, s_aidx = 0;
std::atomic<unsigned> s_vdrop{0}, s_vsent{0};

const int kStepTab[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
                          25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
                          88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
                          307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
                          1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
                          3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
                          12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIdxTab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

uint8_t AdpcmNibble(int sample) {
    const int step = kStepTab[s_aidx];
    int diff = sample - s_apred;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int vpdiff = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        vpdiff += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2;
        diff -= step >> 1;
        vpdiff += step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1;
        vpdiff += step >> 2;
    }
    s_apred += (code & 8) ? -vpdiff : vpdiff;
    if (s_apred > 32767) s_apred = 32767;
    if (s_apred < -32768) s_apred = -32768;
    s_aidx += kIdxTab[code];
    if (s_aidx < 0) s_aidx = 0;
    if (s_aidx > 88) s_aidx = 88;
    return code;
}

void VoiceEnqueue(const VPkt& p) {
    if (!s_vq || xQueueSend(s_vq, &p, 0) != pdTRUE) s_vdrop.fetch_add(1);
}

/** 把缓冲里的 n 个样点编成一包（n 为偶数） */
void VoiceFlushFrame(int n) {
    if (n <= 0) return;
    VPkt p{};
    p.type = 0;
    p.data[0] = static_cast<uint8_t>(s_vseq & 0xFF);
    p.data[1] = static_cast<uint8_t>(s_vseq >> 8);
    ++s_vseq;
    p.data[2] = static_cast<uint8_t>(s_apred & 0xFF);
    p.data[3] = static_cast<uint8_t>((s_apred >> 8) & 0xFF);
    p.data[4] = static_cast<uint8_t>(s_aidx);
    p.data[5] = 0;
    int k = 6;
    for (int i = 0; i + 1 < n; i += 2) {
        const uint8_t lo = AdpcmNibble(s_vbuf[i]);
        const uint8_t hi = AdpcmNibble(s_vbuf[i + 1]);
        p.data[k++] = static_cast<uint8_t>(lo | (hi << 4));
    }
    p.len = static_cast<uint8_t>(k);
    VoiceEnqueue(p);
}

/** 每包样点数：按 MTU 来，最多 20ms（320 点） */
int VoiceFrameSamples() {
    const uint16_t c = s_conn.load();
    const int mtu = c != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(c) : 23;
    int n = (mtu - 3 - 6) * 2;
    if (n > 320) n = 320;
    if (n < 20) n = 20;
    return n & ~1;
}

void Notify() {
    if (s_cb) s_cb();
}
void SetState(State s) {
    s_state.store(s);
    Notify();
}

int GapEvent(ble_gap_event* ev, void*);

void StartAdv() {
    ble_hs_adv_fields f{};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = 0x03C1;  // 键盘
    f.appearance_is_present = 1;
    f.tx_pwr_lvl_is_present = 1;
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f.name = reinterpret_cast<const uint8_t*>(kName);
    f.name_len = std::strlen(kName);
    f.name_is_complete = 1;
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    ble_gap_adv_params p{};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &p, GapEvent, nullptr);
    ESP_LOGI(TAG, "advertising rc=%d", rc);
    if (rc == 0 || rc == BLE_HS_EALREADY) SetState(State::kAdvertising);
}

int GapEvent(ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "connect status=%d", ev->connect.status);
            if (ev->connect.status == 0) {
                s_conn.store(ev->connect.conn_handle);
                s_encrypted.store(false);
                SetState(State::kConnected);
                // 不主动发起加密：手机/电脑点「配对」时自己会发起，两边同时发起会撞车，配对直接失败
            } else {
                StartAdv();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect reason=0x%x", ev->disconnect.reason);
            s_conn.store(BLE_HS_CONN_HANDLE_NONE);
            s_encrypted.store(false);
            s_passkey.store(-1);
            s_audio_sub.store(false);
            if (s_state.load() != State::kOff) StartAdv();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption status=%d", ev->enc_change.status);
            s_encrypted.store(ev->enc_change.status == 0);
            s_auth_fail.store(ev->enc_change.status != 0);
            s_passkey.store(-1);
            if (ev->enc_change.status == 0) {
                // 请主机把连接间隔调到 7.5~15ms（默认常见 30~50ms，鼠标只有 20~30Hz）
                ble_gap_upd_params up{};
                up.itvl_min = 6;    // ×1.25ms
                up.itvl_max = 12;
                up.latency = 0;
                up.supervision_timeout = 300;  // ×10ms
                const int urc = ble_gap_update_params(ev->enc_change.conn_handle, &up);
                ESP_LOGI(TAG, "conn param update req rc=%d", urc);
            }
            Notify();
            return 0;
        case BLE_GAP_EVENT_PARING_COMPLETE:
            ESP_LOGI(TAG, "pairing complete status=0x%x", ev->pairing_complete.status);
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            ESP_LOGI(TAG, "repeat pairing: drop old bond");
            // 主机删了配对又来配：丢掉旧的，按新的配
            ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            ble_sm_io io{};
            io.action = ev->passkey.params.action;
            ESP_LOGI(TAG, "passkey action=%d", io.action);
            if (io.action == BLE_SM_IOACT_DISP) {
                io.passkey = esp_random() % 1000000;
                s_passkey.store(static_cast<int>(io.passkey));
                ESP_LOGI(TAG, "passkey shown on screen");
                Notify();
            } else if (io.action == BLE_SM_IOACT_NUMCMP) {
                // 设备这边直接接受；数字显示在屏上，给用户核对
                s_passkey.store(static_cast<int>(ev->passkey.params.numcmp));
                io.numcmp_accept = 1;
                Notify();
            }
            const int irc = ble_sm_inject_io(ev->passkey.conn_handle, &io);
            if (irc != 0) ESP_LOGW(TAG, "inject io rc=%d", irc);
            return 0;
        }
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (ev->subscribe.attr_handle == s_audio_h) {
                s_audio_sub.store(ev->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "voice host %s", ev->subscribe.cur_notify ? "subscribed" : "unsubscribed");
                Notify();
            }
            return 0;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu=%d", ev->mtu.value);
            return 0;
        case BLE_GAP_EVENT_CONN_UPDATE: {
            ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0)
                ESP_LOGI(TAG, "conn interval=%.2fms latency=%d status=%d", d.conn_itvl * 1.25f, d.conn_latency,
                         ev->conn_update.status);
            return 0;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (s_state.load() == State::kAdvertising) StartAdv();
            return 0;
        default:
            return 0;
    }
}

void HidEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id == ESP_HIDD_START_EVENT) {
        ESP_LOGI(TAG, "hid start");
        if (s_synced) xSemaphoreGive(s_synced);
    }
}

void HostTask(void*) {
    nimble_port_run();  // nimble_port_stop() 后返回
    nimble_port_freertos_deinit();
}

bool Ready() { return s_dev && s_conn.load() != BLE_HS_CONN_HANDLE_NONE && s_encrypted.load(); }

bool Send(uint8_t id, const uint8_t* d, uint8_t len) {
    if (!Ready()) return false;
    return esp_hidd_dev_input_set(s_dev, 0, id, const_cast<uint8_t*>(d), len) == ESP_OK;
}

void TxTask(void*) {
    // 系统节拍是 10ms（CONFIG_FREERTOS_HZ=100），不能用 <10ms 的 vTaskDelay（会变成 0、空转占满 CPU）。
    // 改成「有数据就叫醒」：Mouse()/Enqueue() 里 xTaskNotifyGive，触摸每来一次就发一次
    while (s_tx_run.load()) {
        ulTaskNotifyTake(pdTRUE, 1);
        // 语音包：通知失败（协议栈缓冲满）就留在队里，下一拍再发
        VPkt vp;
        while (s_vq && xQueuePeek(s_vq, &vp, 0) == pdTRUE) {
            const uint16_t c = s_conn.load();
            if (c == BLE_HS_CONN_HANDLE_NONE) {
                xQueueReset(s_vq);
                break;
            }
            os_mbuf* om = ble_hs_mbuf_from_flat(vp.data, vp.len);
            if (!om) break;
            const int nrc = ble_gatts_notify_custom(c, vp.type == 0 ? s_audio_h : s_ctrl_h, om);
            if (nrc != 0) break;  // om 已由协议栈释放
            xQueueReceive(s_vq, &vp, 0);
            s_vsent.fetch_add(1);
        }
        Rpt r;
        while (xQueueReceive(s_q, &r, 0) == pdTRUE) {
            Send(r.id, r.data, r.len);
            if (r.id == kRidKbd || r.id == kRidTouch) vTaskDelay(1);  // 报告之间留一拍，主机才认得出先后
        }
        int dx = s_mdx.exchange(0), dy = s_mdy.exchange(0), wh = s_mwheel.exchange(0);
        const bool btn = s_mbtn_dirty.exchange(false);
        while (dx || dy || wh || btn) {
            const int sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
            const int sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
            const int sw = wh > 127 ? 127 : (wh < -127 ? -127 : wh);
            uint8_t m[4] = {s_mbtn.load(), static_cast<uint8_t>(static_cast<int8_t>(sx)),
                            static_cast<uint8_t>(static_cast<int8_t>(sy)), static_cast<uint8_t>(static_cast<int8_t>(sw))};
            if (!Send(kRidMouse, m, 4)) {
                // 协议栈缓冲满了：位移放回去，下一拍再发，别丢
                s_mdx.fetch_add(dx);
                s_mdy.fetch_add(dy);
                s_mwheel.fetch_add(wh);
                if (btn) s_mbtn_dirty.store(true);
                break;
            }
            dx -= sx;
            dy -= sy;
            wh -= sw;
            if (!dx && !dy && !wh) break;
        }
    }
    s_tx_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void TouchReport(bool down, int x, int y) {
    // x/y：0..10000
    uint8_t d[5] = {static_cast<uint8_t>(down ? 0x03 : 0x02), static_cast<uint8_t>(x & 0xFF),
                    static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(y & 0xFF), static_cast<uint8_t>(y >> 8)};
    Rpt r{kRidTouch, 5, {}};
    std::memcpy(r.data, d, 5);
    xQueueSend(s_q, &r, pdMS_TO_TICKS(50));
}

void Wake() {
    if (TaskHandle_t t = s_tx_task) xTaskNotifyGive(t);
}

void Enqueue(uint8_t id, const uint8_t* d, uint8_t len) {
    if (!s_q || !Ready()) return;
    Rpt r{id, len, {}};
    std::memcpy(r.data, d, len);
    xQueueSend(s_q, &r, pdMS_TO_TICKS(50));
    Wake();
}

// ASCII → HID 用法码（美式布局）；返回 0 表示不支持
uint8_t AsciiUsage(char c, bool& shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') {
        shift = true;
        return 0x04 + (c - 'A');
    }
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    static const char* kPlain = " -=[]\\;'`,./";
    static const uint8_t kPlainU[] = {0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    static const char* kShift = "!@#$%^&*()_+{}|:\"~<>?";
    static const uint8_t kShiftU[] = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D,
                                      0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    for (int i = 0; kPlain[i]; ++i)
        if (kPlain[i] == c) return kPlainU[i];
    for (int i = 0; kShift[i]; ++i)
        if (kShift[i] == c) {
            shift = true;
            return kShiftU[i];
        }
    if (c == '\n') return 0x28;
    if (c == '\t') return 0x2B;
    return 0;
}

}  // namespace

bool Start() {
    std::lock_guard<std::mutex> lk(s_api_mu);
    if (s_state.load() != State::kOff) return true;
    SetState(State::kStarting);
    esp_err_t err = ESP_OK;
    if (!s_mem_released) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        s_mem_released = true;
    }
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err == ESP_OK) err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err == ESP_OK) err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller/nimble init: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        SetState(State::kOff);
        return false;
    }
    // 配对：数字比对（手机/电脑和设备屏上出同一个 6 位数，手机上点确认即可，不用输入）。
    // 之前用「设备显示、手机输入」，手机还没输就报 PIN 不对（SM 错误 0x04 确认值不符）
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    if (!s_synced) s_synced = xSemaphoreCreateBinary();
    xSemaphoreTake(s_synced, 0);
    err = esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE, HidEvent, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hidd init: %s", esp_err_to_name(err));
        esp_nimble_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        s_dev = nullptr;
        SetState(State::kOff);
        return false;
    }
    {
        int grc = ble_gatts_count_cfg(kVoiceSvcs);
        if (grc == 0) grc = ble_gatts_add_svcs(kVoiceSvcs);
        if (grc != 0) ESP_LOGE(TAG, "voice gatt add rc=%d", grc);
    }
    if (!s_vq) s_vq = xQueueCreateWithCaps(40, sizeof(VPkt), MALLOC_CAP_SPIRAM);
    xQueueReset(s_vq);
    ble_svc_gap_device_name_set(kName);
    ble_svc_gap_device_appearance_set(0x03C1);
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    if (!s_q) s_q = xQueueCreate(48, sizeof(Rpt));
    xQueueReset(s_q);
    s_tx_run.store(true);
    // 发送任务不碰 Flash，栈放 PSRAM，给蓝牙控制器省内部 RAM
    xTaskCreatePinnedToCoreWithCaps(TxTask, "ble_tx", 3072, nullptr, 5, &s_tx_task, 1, MALLOC_CAP_SPIRAM);
    esp_nimble_enable(reinterpret_cast<void*>(HostTask));
    if (xSemaphoreTake(s_synced, pdMS_TO_TICKS(3000)) != pdTRUE) ESP_LOGW(TAG, "host sync timeout");
    StartAdv();
    ESP_LOGI(TAG, "started, internal free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lk(s_api_mu);
    if (s_state.load() == State::kOff) return;
    s_state.store(State::kOff);
    const uint16_t c = s_conn.load();
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 20 && s_conn.load() != BLE_HS_CONN_HANDLE_NONE; ++i) vTaskDelay(pdMS_TO_TICKS(25));
    }
    ble_gap_adv_stop();
    s_tx_run.store(false);
    for (int i = 0; i < 20 && s_tx_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_dev) {
        esp_hidd_dev_deinit(s_dev);  // 里面会 ble_gatts_stop() 并释放 GATT 上下文
        s_dev = nullptr;
        // IDF 5.5 的坑：esp_nimble_deinit() 还会再 ble_gatts_stop() 一次，上下文已是空指针就崩。
        // ble_gatts_conn_can_alloc() 只会重新分配一个清零的上下文（不碰已释放的属性表），第二次 stop 就安全了
        ble_gatts_conn_can_alloc();
    }
    nimble_port_stop();
    esp_nimble_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_conn.store(BLE_HS_CONN_HANDLE_NONE);
    s_encrypted.store(false);
    s_passkey.store(-1);
    s_mbtn.store(0);
    ESP_LOGI(TAG, "stopped, internal free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Notify();
}

State GetState() {
    const State s = s_state.load();
    // 连上但还没加密（配对中）在界面上仍算「连接中」
    if (s == State::kConnected && !s_encrypted.load()) return State::kAdvertising;
    return s;
}

int PendingPasskey() { return s_passkey.load(); }

bool AuthFailed() { return s_auth_fail.load(); }

void ForgetPeers() {
    if (s_state.load() == State::kOff) return;
    const uint16_t c = s_conn.load();
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    s_auth_fail.store(false);
    ESP_LOGI(TAG, "bonds cleared");
}

void SetStateCallback(void (*cb)()) { s_cb = cb; }

bool VoiceHostReady() { return Ready() && s_audio_sub.load(); }

void SetVoiceResultCallback(void (*cb)(int status, const char* text)) { s_vresult_cb = cb; }

void VoiceBegin(uint8_t engine) {
    std::lock_guard<std::mutex> lk(s_vmu);
    s_vn = 0;
    s_vseq = 0;
    s_apred = 0;
    s_aidx = 0;
    s_vdrop.store(0);
    s_vsent.store(0);
    VPkt p{};
    p.type = 1;
    p.len = 4;
    p.data[0] = 0x01;
    p.data[1] = 16000 & 0xFF;
    p.data[2] = 16000 >> 8;
    p.data[3] = engine;  // 识别方式：0 电脑本机 1 自己服务器 2 MiMo
    VoiceEnqueue(p);
    s_voice_on.store(true);
    Wake();
    ESP_LOGI(TAG, "voice begin, frame=%d samples", VoiceFrameSamples());
}

void VoicePcm(const int16_t* pcm, size_t n) {
    if (!s_voice_on.load()) return;
    std::lock_guard<std::mutex> lk(s_vmu);
    const int fs = VoiceFrameSamples();
    for (size_t i = 0; i < n; ++i) {
        s_vbuf[s_vn++] = pcm[i];
        if (s_vn >= fs) {
            VoiceFlushFrame(s_vn);
            s_vn = 0;
        }
    }
    Wake();
}

void VoiceEnd() {
    if (!s_voice_on.exchange(false)) return;
    std::lock_guard<std::mutex> lk(s_vmu);
    VoiceFlushFrame(s_vn & ~1);
    s_vn = 0;
    VPkt p{};
    p.type = 1;
    p.len = 1;
    p.data[0] = 0x02;
    VoiceEnqueue(p);
    Wake();
    ESP_LOGI(TAG, "voice end: frames=%u sent=%u dropped=%u", (unsigned)s_vseq, (unsigned)s_vsent.load(),
             (unsigned)s_vdrop.load());
}

void Mouse(int dx, int dy, int wheel, uint8_t buttons) {
    if (!Ready()) return;
    if (buttons != s_mbtn.load()) {
        s_mbtn.store(buttons);
        s_mbtn_dirty.store(true);
    }
    s_mdx.fetch_add(dx);
    s_mdy.fetch_add(dy);
    s_mwheel.fetch_add(wheel);
    Wake();
}

void MouseButtons(uint8_t buttons) {
    if (!Ready()) return;
    // 按键变化走队列，保证「按下 → 松开」不会被合并掉
    s_mbtn.store(buttons);
    uint8_t m[4] = {buttons, 0, 0, 0};
    Enqueue(kRidMouse, m, 4);
}

uint8_t MouseButtonsHeld() { return s_mbtn.load(); }

void KeyTap(uint8_t mod, uint8_t usage) {
    uint8_t down[8] = {mod, 0, usage, 0, 0, 0, 0, 0};
    uint8_t up[8] = {0};
    Enqueue(kRidKbd, down, 8);
    Enqueue(kRidKbd, up, 8);
}

void TypeAscii(const std::string& s) {
    for (char c : s) {
        bool shift = false;
        const uint8_t u = AsciiUsage(c, shift);
        if (u) KeyTap(shift ? 0x02 : 0, u);
    }
}

bool AsciiToUsage(char c, uint8_t& usage, bool& shift) {
    usage = AsciiUsage(c, shift);
    return usage != 0;
}

void Swipe(int x0, int y0, int x1, int y1, int steps) {
    if (!s_q || !Ready()) return;
    if (steps < 2) steps = 2;
    TouchReport(true, x0, y0);
    for (int i = 1; i <= steps; ++i) TouchReport(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    TouchReport(false, x1, y1);
    Wake();
}

void Tap(int x, int y) {
    if (!s_q || !Ready()) return;
    TouchReport(true, x, y);
    TouchReport(false, x, y);
    Wake();
}

void Consumer(uint16_t usage) {
    uint8_t down[2] = {static_cast<uint8_t>(usage & 0xFF), static_cast<uint8_t>(usage >> 8)};
    uint8_t up[2] = {0, 0};
    Enqueue(kRidCc, down, 2);
    Enqueue(kRidCc, up, 2);
}

}  // namespace ble_hid
