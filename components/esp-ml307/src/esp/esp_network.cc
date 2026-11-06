#include "esp_network.h"

#include "esp_tcp.h"
#include "esp_ssl.h"
#include "esp_udp.h"
#include "esp_mqtt.h"
#include "http_client.h"
#include "web_socket.h"


#include <atomic>

static std::atomic<bool> s_blocked{false};
static std::atomic<int> s_live{0};
void EspNetwork_SetBlocked(bool blocked) { s_blocked.store(blocked); }
bool EspNetwork_Blocked() { return s_blocked.load(); }
int EspNetwork_LiveSockets() { return s_live.load(); }
void EspNetwork_SocketCreated() { s_live.fetch_add(1); }
void EspNetwork_SocketDestroyed() { s_live.fetch_sub(1); }

EspNetwork::EspNetwork() {

}

EspNetwork::~EspNetwork() {

}

std::unique_ptr<Http> EspNetwork::CreateHttp(int connect_id) {
    return std::make_unique<HttpClient>(this, connect_id);
}

std::unique_ptr<Tcp> EspNetwork::CreateTcp(int connect_id) {
    return std::make_unique<EspTcp>();
}

std::unique_ptr<Tcp> EspNetwork::CreateSsl(int connect_id) {
    return std::make_unique<EspSsl>();
}

std::unique_ptr<Udp> EspNetwork::CreateUdp(int connect_id) {
    return std::make_unique<EspUdp>();
}

std::unique_ptr<Mqtt> EspNetwork::CreateMqtt(int connect_id) {
    return std::make_unique<EspMqtt>();
}

std::unique_ptr<WebSocket> EspNetwork::CreateWebSocket(int connect_id) {
    return std::make_unique<WebSocket>(this, connect_id);
}