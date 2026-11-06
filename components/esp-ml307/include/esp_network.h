#ifndef ESP_NETWORK_H
#define ESP_NETWORK_H

#include "network_interface.h"

class EspNetwork : public NetworkInterface {
public:
    EspNetwork();
    ~EspNetwork();

    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) override;
    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override;
    std::unique_ptr<Udp> CreateUdp(int connect_id = -1) override;
    std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) override;
    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id = -1) override;
};

/**
 * 网络闸门：关掉后新连接一律失败（主程序拆 WiFi 前先关，避免 netif 被拆时还有连接在建，lwip 崩）。
 * 活着的 TCP/TLS 对象计数，拆 WiFi 前等它归零（各下载任务失败退出）。
 */
void EspNetwork_SetBlocked(bool blocked);
bool EspNetwork_Blocked();
int EspNetwork_LiveSockets();
void EspNetwork_SocketCreated();
void EspNetwork_SocketDestroyed();

#endif // ESP_NETWORK_H
