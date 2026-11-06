#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <dirent.h>
#include "boards/metalio-e-ink-4/bus/metalio_i2c_bus.h"
#include "sc7a20h.h"
#include "vk_key_handler.h"
#include "SimpleUart.hpp"
#include <driver/uart.h>
#include "ble_hid/ble_hid.h"
#include "blehid_screen/blehid_screen.h"
#include <sys/stat.h>
#include "sd_paths.h"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <cerrno>
#include "screen/translate_screen/translate_screen.h"
#include "lvgl_display/jpg/jpeg_to_image.h"
#include "lvgl_display/jpg/image_to_jpeg.h"
#include <multi_heap.h>
#include <esp_memory_utils.h>
#include <cstdlib>
#include "screen/standby_screen/standby_screen.h"
#include "screen/monitor_screen/monitor_screen.h"
#include "screen/standby_screen/standby_card.h"
#include "battery_log.h"
#include "board.h"
#include "feedback.h"
#include "power_policy.h"
#include "screen/gallery_screen/gallery_screen.h"
#include "net_mode.h"
#include "screen/boot_key_handler.h"
#include <cJSON.h>
#include "screen_dump.h"
#include "application.h"

#include "display_orient.h"
#include "lv_adapter_display.h"
#include "my_ai_screen/my_ai_screen.h"
#include "my_ai_screen/my_ai_history.h"
#include "weread_screen/weread_screen.h"
#include "screen_common.h"
#include "quick_panel.h"
#include "settings_screen/settings_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <hal/usb_serial_jtag_ll.h>
#include <mbedtls/base64.h>

extern "C" uint8_t g_ssd1677_partial_ctrl2;
extern "C" uint8_t g_ssd1677_gray_ctrl2;
extern "C" int g_ssd1677_gray_vcom_after;  // 面板驱动：局刷 0x22 参数（调试）

namespace {

std::string ReadSmallFile(const char* path) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < 16384) out.append(buf, n);
    std::fclose(f);
    return out;
}

// 把 SD my_ai.json 里的 glm_api_key 直接发给自建服务器（串口不回显 Key）
void KeyPushTask(void*) {
    ESP_LOGW("KeyPush", "start");
    std::string server, token, key;
    if (cJSON* w = cJSON_Parse(ReadSmallFile("/sdcard/metalio/e-ink/weread.json").c_str())) {
        const cJSON* sv = cJSON_GetObjectItemCaseSensitive(w, "server");
        const cJSON* tk = cJSON_GetObjectItemCaseSensitive(w, "token");
        if (cJSON_IsString(sv)) server = sv->valuestring;
        if (cJSON_IsString(tk)) token = tk->valuestring;
        cJSON_Delete(w);
    }
    if (cJSON* m = cJSON_Parse(ReadSmallFile("/sdcard/metalio/e-ink/my_ai.json").c_str())) {
        const cJSON* k = cJSON_GetObjectItemCaseSensitive(m, "glm_api_key");
        if (cJSON_IsString(k)) key = k->valuestring;
        cJSON_Delete(m);
    }
    while (!server.empty() && server.back() == '/') server.pop_back();
    int status = -1;
    if (!server.empty() && !token.empty() && !key.empty()) {
        auto* network = Board::GetInstance().GetNetwork();
        auto http = network ? network->CreateHttp(0) : nullptr;
        if (http) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "key", key.c_str());
            cJSON_AddStringToObject(body, "vendor", "glm");
            char* js = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            http->SetTimeout(15000);
            http->SetHeader("X-Token", token);
            http->SetHeader("Content-Type", "application/json");
            http->SetContent(std::string(js));
            cJSON_free(js);
            if (http->Open("POST", server + "/api/llmkey")) {
                status = http->GetStatusCode();
                http->ReadAll();
                http->Close();
            }
        }
    }
    ESP_LOGW("KeyPush", "keypush status=%d key_len=%u", status, static_cast<unsigned>(key.size()));
    vTaskDeleteWithCaps(nullptr);
}


constexpr const char* TAG = "ScreenDump";
constexpr size_t kChunkBytes = 768;  // base64 后 1024 字符一行

std::atomic<bool> s_started{false};

void WriteAll(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t n = write(fd, data, len);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void DumpOnce(int fd) {
    auto* disp = LVAdapterDisplay::Instance();
    std::vector<uint8_t> fb;
    int w = 0, h = 0;
    if (disp == nullptr || !disp->CopyPanelFrame(fb, &w, &h)) {
        WriteAll(fd, "@@SHOT ERR no-frame\n", 20);
        return;
    }
    const uint32_t crc = esp_rom_crc32_le(0, fb.data(), fb.size());
    // PackBits：墨水屏大片纯白，压缩后通常只剩原来的 1/5～1/10
    std::vector<uint8_t> packed;
    packed.reserve(fb.size() / 4);
    for (size_t i = 0; i < fb.size();) {
        size_t run = 1;
        while (i + run < fb.size() && run < 128 && fb[i + run] == fb[i]) ++run;
        if (run >= 3) {
            packed.push_back(static_cast<uint8_t>(257 - run));  // -(run-1)
            packed.push_back(fb[i]);
            i += run;
            continue;
        }
        const size_t lit_start = i;
        size_t lit = 0;
        while (i < fb.size() && lit < 128) {
            if (i + 2 < fb.size() && fb[i] == fb[i + 1] && fb[i] == fb[i + 2]) break;
            ++i;
            ++lit;
        }
        packed.push_back(static_cast<uint8_t>(lit - 1));
        packed.insert(packed.end(), fb.begin() + lit_start, fb.begin() + lit_start + lit);
    }
    char head[96];
    // 开头补换行：别的任务的半行日志不会粘到头上
    const int hn = std::snprintf(head, sizeof(head), "\n@@SHOT BEGIN %d %d %u %d %08lx packbits %u\n", w, h,
                                 static_cast<unsigned>(fb.size()), DisplayUiGetOrient(),
                                 static_cast<unsigned long>(crc), static_cast<unsigned>(packed.size()));
    WriteAll(fd, head, static_cast<size_t>(hn));

    std::string line;
    line.reserve(16 + 1100);
    unsigned char b64[1100];
    for (size_t off = 0, idx = 0; off < packed.size(); off += kChunkBytes, ++idx) {
        const size_t n = std::min(kChunkBytes, packed.size() - off);
        size_t olen = 0;
        mbedtls_base64_encode(b64, sizeof(b64), &olen, packed.data() + off, n);
        line.assign("\n@@S ");
        line += std::to_string(idx);
        line.push_back(' ');
        line.append(reinterpret_cast<const char*>(b64), olen);
        line.push_back('\n');
        WriteAll(fd, line.data(), line.size());
    }
    WriteAll(fd, "\n@@SHOT END\n", 12);
    ESP_LOGI(TAG, "dumped %dx%d %u -> %u bytes", w, h, static_cast<unsigned>(fb.size()),
             static_cast<unsigned>(packed.size()));
}

// 串口传文件到 SD：FPUT <相对 SD_APP_ROOT 的路径> / FPB <base64> ... / FPE
FILE* s_fput = nullptr;
std::string s_fput_path;
size_t s_fput_bytes = 0;

void PollTask(void*) {
    int fd = -1;
    std::string cmd;
    while (true) {
        // 传文件时不睡：USB FIFO 只有 64 字节，100ms 一轮的话一秒才收几百字节
        if (s_fput) taskYIELD(); else vTaskDelay(pdMS_TO_TICKS(100));
        uint8_t buf[32];
        while (usb_serial_jtag_ll_rxfifo_data_available()) {
            const uint32_t n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
            if (n == 0) break;
            for (uint32_t i = 0; i < n; ++i) {
                const char c = static_cast<char>(buf[i]);
                if (c == '\n' || c == '\r') {
                    if (cmd.rfind("FPB ", 0) == 0) {
                        if (s_fput) {
                            static uint8_t out[1024];
                            size_t olen = 0;
                            if (mbedtls_base64_decode(out, sizeof(out), &olen,
                                                      reinterpret_cast<const unsigned char*>(cmd.data() + 4),
                                                      cmd.size() - 4) == 0) {
                                std::fwrite(out, 1, olen, s_fput);
                                s_fput_bytes += olen;
                            } else {
                                ESP_LOGW(TAG, "FPB bad base64");
                            }
                        }
                    } else if (cmd.rfind("FPUT ", 0) == 0) {
                        if (s_fput) std::fclose(s_fput);
                        s_fput_path = std::string(SD_APP_ROOT) + "/" + cmd.substr(5);
                        const size_t sl = s_fput_path.rfind('/');
                        if (sl != std::string::npos) mkdir(s_fput_path.substr(0, sl).c_str(), 0775);
                        s_fput = std::fopen((s_fput_path + ".tmp").c_str(), "wb");
                        s_fput_bytes = 0;
                        ESP_LOGW(TAG, "FPUT %s %s", s_fput_path.c_str(), s_fput ? "open" : "FAIL");
                    } else if (cmd == "FEEDBACK") {
                        feedback::Capture("serial");
                    } else if (cmd.rfind("DRAG ", 0) == 0) {
                        int a = 0, b = 0, c = 0, d = 0, ms = 300;
                        if (std::sscanf(cmd.c_str() + 5, "%d %d %d %d %d", &a, &b, &c, &d, &ms) >= 4) LVAdapterDisplay::DebugRawDrag(a, b, c, d, ms);
                    } else if (cmd.rfind("TAP ", 0) == 0) {
                        int x = 0, y = 0, ms = 80;
                        if (std::sscanf(cmd.c_str() + 4, "%d %d %d", &x, &y, &ms) >= 2) LVAdapterDisplay::DebugTap(x, y, ms);
                    } else if (cmd.rfind("PLUT ", 0) == 0) {
                        int a = 0, b = -1, c = 0;
                        if (std::sscanf(cmd.c_str() + 5, "%d %d %i", &a, &b, &c) >= 1) LVAdapterDisplay::SetGrayLut(4, a, b, c, 0, 0);
                    } else if (cmd.rfind("PGVCOM ", 0) == 0) {
                        g_ssd1677_gray_vcom_after = static_cast<int>(std::strtol(cmd.c_str() + 7, nullptr, 0));
                        ESP_LOGW(TAG, "gray vcom after=%d", g_ssd1677_gray_vcom_after);
                    } else if (cmd.rfind("PGRAY22 ", 0) == 0) {
                        g_ssd1677_gray_ctrl2 = static_cast<uint8_t>(std::strtol(cmd.c_str() + 8, nullptr, 0));
                        ESP_LOGW(TAG, "gray ctrl2=0x%02X", g_ssd1677_gray_ctrl2);
                    } else if (cmd.rfind("PCTRL2 ", 0) == 0) {
                        g_ssd1677_partial_ctrl2 = static_cast<uint8_t>(std::strtol(cmd.c_str() + 7, nullptr, 0));
                        ESP_LOGW(TAG, "partial ctrl2=0x%02X", g_ssd1677_partial_ctrl2);
                    } else if (cmd.rfind("PFLASH ", 0) == 0) {
                        LVAdapterDisplay::SetGrayLut(5, std::atoi(cmd.c_str() + 7), 0, 0, 0, 0);
                    } else if (cmd.rfind("FLS ", 0) == 0) {
                        // 列 SD 目录：@@FLS name size@@ 每行一条
                        const std::string dir = cmd.substr(4);
                        if (DIR* d = opendir(dir.c_str())) {
                            while (struct dirent* e = readdir(d)) {
                                struct stat st {};
                                const std::string fp = dir + "/" + e->d_name;
                                stat(fp.c_str(), &st);
                                printf("\n@@FLS %s %ld %d@@\n", e->d_name, static_cast<long>(st.st_size), S_ISDIR(st.st_mode) ? 1 : 0);
                            }
                            closedir(d);
                        }
                        printf("\n@@FLS END@@\n");
                    } else if (cmd.rfind("FGET ", 0) == 0) {
                        // 读回 SD 小文件（≤4KB），base64 一行输出，供电脑端脚本直接存文件
                        // FGET <路径> [偏移]：每次最多 4KB，电脑端按偏移循环取大文件（反馈的日志/截屏）
                        std::string rest = cmd.substr(5);
                        long off = 0;
                        const size_t sp = rest.rfind(' ');
                        if (sp != std::string::npos && sp + 1 < rest.size() &&
                            std::strspn(rest.c_str() + sp + 1, "0123456789") == rest.size() - sp - 1) {
                            off = std::atol(rest.c_str() + sp + 1);
                            rest.resize(sp);
                        }
                        const std::string pth = std::string(SD_APP_ROOT) + "/" + rest;
                        static uint8_t fin[4096];
                        static unsigned char fout[5600];
                        size_t len = 0, olen = 0;
                        long total = -1;
                        if (FILE* f = std::fopen(pth.c_str(), "rb")) {
                            std::fseek(f, 0, SEEK_END);
                            total = std::ftell(f);
                            std::fseek(f, off, SEEK_SET);
                            len = std::fread(fin, 1, sizeof(fin), f);
                            std::fclose(f);
                            mbedtls_base64_encode(fout, sizeof(fout), &olen, fin, len);
                        }
                        fout[olen] = 0;
                        printf("\n@@FGET %u %s@@ total=%ld\n", static_cast<unsigned>(len), fout, total);
                        fflush(stdout);
                    } else if (cmd.rfind("FRM ", 0) == 0) {
                        const std::string pth = std::string(SD_APP_ROOT) + "/" + cmd.substr(4);
                        ESP_LOGW(TAG, "FRM %s rc=%d", pth.c_str(), unlink(pth.c_str()));
                    } else if (cmd == "FPE") {
                        if (s_fput) {
                            std::fclose(s_fput);
                            s_fput = nullptr;
                            unlink(s_fput_path.c_str());
                            const int rc = rename((s_fput_path + ".tmp").c_str(), s_fput_path.c_str());
                            ESP_LOGW(TAG, "FPUT done %s %u bytes rc=%d", s_fput_path.c_str(),
                                     static_cast<unsigned>(s_fput_bytes), rc);
                        }
                    } else if (cmd == "SHOT") {
                        if (fd < 0) fd = open("/dev/secondary", O_WRONLY);
                        if (fd >= 0) DumpOnce(fd);
                    } else if (cmd.rfind("ASK ", 0) == 0 && cmd.size() == 5) {
                        MyAiScreen::DebugAsk(cmd[4] - '0');
                    } else if (cmd.rfind("Q ", 0) == 0) {
                        MyAiScreen::DebugAskText(cmd.c_str() + 2);
                    } else if (cmd.rfind("MODEL ", 0) == 0 && cmd.size() == 7) {
                        MyAiScreen::DebugSelectModel(cmd[6] - '0');
                    } else if (cmd.rfind("LOAD ", 0) == 0 && cmd.size() == 6) {
                        static int s_load_idx;
                        s_load_idx = cmd[5] - '0';
                        ScreenLvAsync([](void*) { MyAiScreen::DebugLoadHistory(s_load_idx); });
                    } else if (cmd == "OPEN WEREAD") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(WeReadScreen::Create); });
                    } else if (cmd.rfind("WRCFG ", 0) == 0) {
                        // 写 SD 卡 weread.json（内容为一行 JSON），不回显内容
                        FILE* f = std::fopen("/sdcard/metalio/e-ink/weread.json", "wb");
                        if (f) {
                            std::fwrite(cmd.data() + 6, 1, cmd.size() - 6, f);
                            std::fclose(f);
                            ESP_LOGI(TAG, "weread.json written (%u bytes)", static_cast<unsigned>(cmd.size() - 6));
                        } else {
                            ESP_LOGW(TAG, "weread.json open failed");
                        }
                    } else if (cmd.rfind("WR ", 0) == 0) {
                        static char s_wr_cmd[64];
                        std::snprintf(s_wr_cmd, sizeof(s_wr_cmd), "%s", cmd.c_str() + 3);
                        ScreenLvAsync([](void*) { WeReadScreen::DebugCmd(s_wr_cmd); });
                    } else if (cmd == "AIAGE") {
                        // 调试：把所有本地对话的访问时间改到 8 天前，下次同步应只留云端
                        my_ai::DebugAgeAll(8 * 24 * 3600);
                        my_ai::CloudSyncAsync();
                    } else if (cmd == "AIOV") {
                        MyAiScreen::RequestOverlay(false);
                    } else if (cmd == "AISHOT") {
                        MyAiScreen::RequestOpenWithShot();
                    } else if (cmd == "OPEN TR") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(TranslateScreen::Create); });
                    } else if (cmd.rfind("TR ", 0) == 0) {
                        static char s_tr_cmd[240];
                        std::snprintf(s_tr_cmd, sizeof(s_tr_cmd), "%s", cmd.c_str() + 3);
                        ScreenLvAsync([](void*) { TranslateScreen::DebugCmd(s_tr_cmd); });
                    } else if (cmd == "OPEN MON") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(MonitorScreen::Create); });
                    } else if (cmd.rfind("MON ", 0) == 0 && cmd.size() == 5) {
                        static char s_mon_c;
                        s_mon_c = cmd[4];
                        ScreenLvAsync([](void*) { MonitorScreen::DebugCmd(s_mon_c); });
                    } else if (cmd == "NOSLEEP") {
                        StandbyCard_DebugNoSleep(true);
                        ESP_LOGW(TAG, "standby light sleep disabled (debug)");
                    } else if (cmd == "BATLOG") {
                        Application::GetInstance().Schedule([]() { BatteryLog_Sample(); });
                    } else if (cmd == "JPEGTEST") {
                        // 编码一张 480x800 灰度测试图再解码回来，校验并计时（jpeg 库从 IRAM 挪到 Flash 后的回归测试）
                        xTaskCreateWithCaps([](void*) {
                            const int w = 480, h = 800;
                            auto* g = static_cast<uint8_t*>(heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM));
                            if (g) {
                                for (int y = 0; y < h; ++y)
                                    for (int x = 0; x < w; ++x) g[y * w + x] = static_cast<uint8_t>(((x / 40) + (y / 40)) & 1 ? 230 : 30);
                                for (int round = 0; round < 3; ++round) {
                                    uint8_t* jpg = nullptr;
                                    size_t jl = 0;
                                    int64_t t0 = esp_timer_get_time();
                                    const bool ok = image_to_jpeg(g, w * h, w, h, V4L2_PIX_FMT_GREY, 70, &jpg, &jl);
                                    const int enc = static_cast<int>((esp_timer_get_time() - t0) / 1000);
                                    uint8_t* rgb = nullptr;
                                    size_t ol = 0, ow = 0, oh = 0, st = 0;
                                    t0 = esp_timer_get_time();
                                    const esp_err_t de = ok ? jpeg_to_image(jpg, jl, &rgb, &ol, &ow, &oh, &st) : ESP_FAIL;
                                    const int dec = static_cast<int>((esp_timer_get_time() - t0) / 1000);
                                    // 抽查几个像素：RGB565 里黑格/白格应明显不同
                                    int bad = 0;
                                    if (de == ESP_OK && rgb && ow == static_cast<size_t>(w)) {
                                        const uint16_t* px = reinterpret_cast<const uint16_t*>(rgb);
                                        for (int k = 0; k < 20; ++k) {
                                            const int x = 20 + 40 * (k % 10), y = 20 + 40 * (k / 10);
                                            const bool light = ((x / 40) + (y / 40)) & 1;
                                            const int lum = (px[y * (st / 2) + x] >> 11) & 0x1F;
                                            if (light ? lum < 20 : lum > 10) ++bad;
                                        }
                                    }
                                    ESP_LOGW("JpegTest", "round %d enc=%s %uB %dms dec=%s %ux%u %dms bad_px=%d", round,
                                             ok ? "ok" : "FAIL", static_cast<unsigned>(jl), enc, esp_err_to_name(de),
                                             static_cast<unsigned>(ow), static_cast<unsigned>(oh), dec, bad);
                                    if (jpg) free(jpg);
                                    if (rgb) heap_caps_free(rgb);
                                }
                                heap_caps_free(g);
                            }
                            vTaskDeleteWithCaps(nullptr);
                        }, "jpegtest", 8192, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    } else if (cmd == "NETDIAG") {
                        // 网络诊断：AP 信号、IP/网关/DNS；再试连网关 80 口和 223.5.5.5:53
                        wifi_ap_record_t ap{};
                        const esp_err_t ae = esp_wifi_sta_get_ap_info(&ap);
                        esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                        esp_netif_ip_info_t ip{};
                        esp_netif_dns_info_t dns{};
                        if (nif) {
                            esp_netif_get_ip_info(nif, &ip);
                            esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns);
                        }
                        ESP_LOGW("NetDiag", "ap=%s rssi=%d err=%s ip=" IPSTR " gw=" IPSTR " dns=" IPSTR,
                                 ae == ESP_OK ? reinterpret_cast<const char*>(ap.ssid) : "-", ae == ESP_OK ? ap.rssi : 0,
                                 esp_err_to_name(ae), IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&dns.ip.u_addr.ip4));
                        auto try_tcp = [](uint32_t addr, int port) {
                            int fd = socket(AF_INET, SOCK_STREAM, 0);
                            if (fd < 0) return -100 - errno;
                            struct timeval tv{3, 0};
                            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                            sockaddr_in sa{};
                            sa.sin_family = AF_INET;
                            sa.sin_port = htons(port);
                            sa.sin_addr.s_addr = addr;
                            const int rc = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
                            const int e = rc == 0 ? 0 : errno;
                            close(fd);
                            return e;
                        };
                        ESP_LOGW("NetDiag", "tcp gw:53 -> %d, 223.5.5.5:53 -> %d, 101.201.41.94:8080 -> %d",
                                 try_tcp(ip.gw.addr, 53), try_tcp(inet_addr("223.5.5.5"), 53),
                                 try_tcp(inet_addr("101.201.41.94"), 8080));
                    } else if (cmd == "WIFIPS 0" || cmd == "WIFIPS 1") {
                        const esp_err_t e = esp_wifi_set_ps(cmd == "WIFIPS 0" ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
                        wifi_ps_type_t t = WIFI_PS_NONE;
                        esp_wifi_get_ps(&t);
                        ESP_LOGW("NetDiag", "wifi ps -> %d (%s)", static_cast<int>(t), esp_err_to_name(e));
                    } else if (cmd == "OFFLINE 1" || cmd == "OFFLINE 0") {
                        const bool on = cmd == "OFFLINE 1";
                        Application::GetInstance().Schedule([on]() { net_mode::SetOffline(on); });
                    } else if (cmd == "BTSWAP 1" || cmd == "BTSWAP 0") {
                        const bool sw = cmd[7] == '1';
                        const esp_err_t e = uart_set_pin(UART_NUM_2, sw ? 47 : 48, sw ? 48 : 47, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
                        ESP_LOGW("BTAT", "pins swap=%d: %s", sw, esp_err_to_name(e));
                    } else if (cmd.rfind("BTBAUD ", 0) == 0) {
                        const int baud = std::atoi(cmd.c_str() + 7);
                        ESP_LOGW("BTAT", "baud -> %d: %s", baud, esp_err_to_name(uart_set_baudrate(UART_NUM_2, baud)));
                    } else if (cmd.rfind("BTAT ", 0) == 0) {
                        // 调试：给外置蓝牙音频模块发 AT，回包打到日志
                        auto& u = SimpleUart::getInstance();
                        {  // 每次都重新挂（设置页蓝牙 tab 会把回调换成自己的）
                            u.registerCallback([](const std::vector<uint8_t>& d) {
                                std::string t(d.begin(), d.end());
                                for (auto& ch : t)
                                    if (ch == '\r' || ch == '\n') ch = '|';
                                ESP_LOGW("BTAT", "rx(%u): %s", (unsigned)d.size(), t.c_str());
                            });
                        }
                        const std::string at = cmd.substr(5) + "\r\n";
                        ESP_LOGW("BTAT", "tx: %s init=%d", cmd.c_str() + 5, u.isInitialized());
                        u.sendString(at);
                    } else if (cmd == "VK next" || cmd == "VK prev" || cmd == "VK home") {
                        static char s_vk[16];
                        std::snprintf(s_vk, sizeof(s_vk), "vk_%s", cmd.c_str() + 3);
                        ScreenLvAsync([](void*) { VkKey_Dispatch(s_vk); });
                    } else if (cmd == "VOICE 1" || cmd == "VOICE 0") {
                        BleHidScreen::DebugTalk(cmd[6] == '1');
                    } else if (cmd == "OPEN BLE") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(BleHidScreen::Create); });
                    } else if (cmd.rfind("BLE t ", 0) == 0) {
                        ble_hid::TypeAscii(cmd.substr(6));
                    } else if (cmd.rfind("BLE m ", 0) == 0) {
                        int dx = 0, dy = 0, wh = 0;
                        std::sscanf(cmd.c_str() + 6, "%d %d %d", &dx, &dy, &wh);
                        ble_hid::Mouse(dx, dy, wh, ble_hid::MouseButtonsHeld());
                    } else if (cmd.rfind("BLE c ", 0) == 0) {
                        ble_hid::Consumer(static_cast<uint16_t>(std::strtol(cmd.c_str() + 6, nullptr, 0)));
                    } else if (cmd == "BLE st") {
                        ESP_LOGW("BleHid", "state=%d passkey=%d", static_cast<int>(ble_hid::GetState()),
                                 ble_hid::PendingPasskey());
                    } else if (cmd.rfind("BLEAXIS ", 0) == 0) {
                        int sw = 0, sx = 1, sy = 1;
                        std::sscanf(cmd.c_str() + 8, "%d %d %d", &sw, &sx, &sy);
                        BleHidScreen::SetTiltAxes(sw != 0, sx, sy);
                    } else if (cmd == "I2CSCAN") {
                        if (auto bus = MetalioI2cBus_Get()) {
                            std::string found;
                            for (int a = 0x08; a < 0x78; ++a) {
                                if (i2c_master_probe(bus, a, 20) == ESP_OK) {
                                    char t[8];
                                    std::snprintf(t, sizeof(t), " 0x%02X", a);
                                    found += t;
                                }
                            }
                            ESP_LOGW("I2C", "scan:%s", found.c_str());
                        }
                    } else if (cmd == "ACCEL") {
                        auto& acc = Sc7a20h::GetInstance();
                        for (int i = 0; i < 5; ++i) {
                            int ax = 0, ay = 0, az = 0;
                            const bool ok = acc.ReadAccelMg(ax, ay, az);
                            ESP_LOGW("ACCEL", "ready=%d ok=%d mg=%d,%d,%d", acc.IsReady(), ok, ax, ay, az);
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    } else if (cmd == "OPEN GAL") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(GalleryScreen::Create); });
                    } else if (cmd.rfind("GLUT ", 0) == 0) {
                        int w = 0, a = 0, b = 0, c = 0, d = 0, fr = 0;
                        if (std::sscanf(cmd.c_str() + 5, "%d %d %d %d %d %i", &w, &a, &b, &c, &d, &fr) >= 3) {
                            LVAdapterDisplay::SetGrayLut(w, a, b, c, d, fr);
                        }
                    } else if (cmd.rfind("GAL ", 0) == 0 && cmd.size() == 5) {
                        GalleryScreen::DebugCmd(cmd[4]);
                    } else if (cmd == "PWRKEY") {
                        PowerKey_OnClick();  // 模拟电源键短按
                    } else if (cmd == "BGHOLD 1") {
                        PowerPolicy::GetInstance().Acquire(PowerNeed::BgDownload);
                        ESP_LOGW("NetDiag", "bg hold +1");
                    } else if (cmd == "BGHOLD 0") {
                        PowerPolicy::GetInstance().Release(PowerNeed::BgDownload);
                        ESP_LOGW("NetDiag", "bg hold -1");
                    } else if (cmd == "WIFIRECON") {
                        ESP_LOGW("NetDiag", "force wifi reconnect");
                        esp_wifi_disconnect();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        esp_wifi_connect();
                    } else if (cmd == "MEMINFO") {
                        // 内部 RAM：堆总况 + 哪些任务栈放在内部 RAM
                        multi_heap_info_t hi{};
                        heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
                        ESP_LOGW("Mem", "internal total=%u free=%u largest=%u minfree=%u",
                                 static_cast<unsigned>(hi.total_free_bytes + hi.total_allocated_bytes),
                                 static_cast<unsigned>(hi.total_free_bytes), static_cast<unsigned>(hi.largest_free_block),
                                 static_cast<unsigned>(hi.minimum_free_bytes));
                        ESP_LOGW("Mem", "dma free=%u  psram free=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                        const UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
                        auto* arr = static_cast<TaskStatus_t*>(heap_caps_malloc(sizeof(TaskStatus_t) * n, MALLOC_CAP_SPIRAM));
                        if (arr) {
                            const UBaseType_t got = uxTaskGetSystemState(arr, n, nullptr);
                            unsigned total_int = 0;
                            for (UBaseType_t i = 0; i < got; ++i) {
                                const uintptr_t sb = reinterpret_cast<uintptr_t>(arr[i].pxStackBase);
                                const bool internal = esp_ptr_internal(arr[i].pxStackBase);
                                const unsigned sz = static_cast<unsigned>(heap_caps_get_allocated_size(arr[i].pxStackBase));
                                if (internal) total_int += sz;
                                ESP_LOGW("Mem", "task %-16s %s stack=%u hwm=%u", arr[i].pcTaskName, internal ? "INT" : "ps ",
                                         sz, static_cast<unsigned>(arr[i].usStackHighWaterMark));
                                (void)sb;
                            }
                            ESP_LOGW("Mem", "tasks=%u internal stacks total=%u", static_cast<unsigned>(got), total_int);
                            // 内部堆里 ≥1KB 的已用块（不知道归属，看大小猜）
                            struct Acc { unsigned big_n, big_sum, small_n, small_sum; unsigned sizes[48]; int k; } acc{};
                            heap_caps_walk(MALLOC_CAP_INTERNAL, [](walker_heap_into_t, walker_block_info_t b, void* u) {
                                auto* a = static_cast<Acc*>(u);
                                if (!b.used) return true;
                                if (b.size >= 1024) {
                                    a->big_n++; a->big_sum += b.size;
                                    if (a->k < 48) a->sizes[a->k++] = static_cast<unsigned>(b.size);
                                } else { a->small_n++; a->small_sum += b.size; }
                                return true;
                            }, &acc);
                            std::string line;
                            for (int i = 0; i < acc.k; ++i) line += std::to_string(acc.sizes[i]) + " ";
                            ESP_LOGW("Mem", "used blocks >=1K: n=%u sum=%u | <1K: n=%u sum=%u", acc.big_n, acc.big_sum,
                                     acc.small_n, acc.small_sum);
                            ESP_LOGW("Mem", "big sizes: %s", line.c_str());
                            heap_caps_free(arr);
                        }
                    } else if (cmd == "STBDBG") {
                        StandbyCard_DbgDump();
                    } else if (cmd.rfind("STBTEST ", 0) == 0) {
                        StandbyCard_DebugAutoExit(std::atoi(cmd.c_str() + 8));
                        ScreenLvAsync([](void*) { StandbyScreen::Show(); });
                    } else if (cmd == "BATDUMP") {
                        auto v = BatteryLog_Read(0);
                        const size_t from = v.size() > 60 ? v.size() - 60 : 0;
                        for (size_t i = from; i < v.size(); ++i) {
                            ESP_LOGW("BatDump", "%ld,%d,%d,%d,%d", static_cast<long>(v[i].t), v[i].level, v[i].mv,
                                     v[i].ma, v[i].chg ? 1 : 0);
                        }
                        ESP_LOGW("BatDump", "total %u", static_cast<unsigned>(v.size()));
                    } else if (cmd == "STANDBY") {
                        ScreenLvAsync([](void*) { StandbyScreen::Show(); });
                    } else if (cmd == "KEYPUSH") {
                        ESP_LOGW(TAG, "keypush cmd");
                        if (xTaskCreateWithCaps(KeyPushTask, "keypush", 8192, nullptr, 5, nullptr,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                            ESP_LOGW("KeyPush", "task create failed");
                        }
                    } else if (cmd == "OTACHECK") {
                        Application::GetInstance().RequestOtaPollNow();
                    } else if (cmd.rfind("OTACFG ", 0) == 0) {
                        FILE* f = std::fopen("/sdcard/metalio/e-ink/ota.json", "wb");
                        if (f) {
                            std::fwrite(cmd.data() + 7, 1, cmd.size() - 7, f);
                            std::fclose(f);
                            ESP_LOGI(TAG, "ota.json written");
                        }
                    } else if (cmd == "FULL") {
                        ScreenLvAsync([](void*) {
                            if (auto* d = LVAdapterDisplay::Instance()) d->RequestFullRefreshNow();
                        });
                    } else if (cmd == "HOME") {
                        ScreenLvAsync([](void*) { ScreenNavigateBack(); });
                    } else if (cmd == "EPDINFO") {
                        if (auto* d = LVAdapterDisplay::Instance()) ESP_LOGW(TAG, "%s", d->DebugInfo().c_str());
                    } else if (cmd == "PANEL") {
                        QuickPanel::RequestOpen();
                    } else if (cmd == "OPEN THEME") {
                        ScreenLvAsync([](void*) { ScreenNavigateTo(SettingsScreen::CreateTheme); });
                    } else if (cmd == "HIST") {
                        ScreenLvAsync([](void*) { MyAiScreen::DebugOpenHistory(); });
                    } else if (cmd == "PICK") {
                        ScreenLvAsync([](void*) { MyAiScreen::DebugOpenPicker(); });
                    } else if (cmd == "OPEN MYAI") {
                        // 调试：直接进「我的 AI」，便于远程看界面
                        ScreenLvAsync([](void*) {
                            if (!MyAiScreen::IsActive()) ScreenNavigateTo(MyAiScreen::Create);
                        });
                    }
                    cmd.clear();
                } else if (cmd.size() < 1100) {  // FPB 一行 512 字符 base64
                    cmd.push_back(c);
                }
            }
        }
    }
}

}  // namespace

extern "C" void ScreenDump_Start(void) {
    if (s_started.exchange(true)) {
        return;
    }
    if (xTaskCreateWithCaps(PollTask, "screen_dump", 4096, nullptr, 1, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "create task failed");
        s_started.store(false);
    }
}
