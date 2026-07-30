#ifndef SRC_PROTOCOL_CP_CP_SERVER
#define SRC_PROTOCOL_CP_CP_SERVER

// The CarPlay control TCP server: listens on :7000, and for each connection
// runs a ControlChannel, pumping bytes between the socket and the channel
// (which handles RTSP framing, the pairing/auth dispatch, and the switch to
// encrypted framing). Transport for wireless CarPlay directly, and for wired
// once USB-NCM brings the IP link up. Pair with the mDNS responder so the phone
// finds it.

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <netinet/in.h>

#include "cp_auth_setup.h"
#include "cp_av.h"

namespace cp_server
{
class Server
{
public:
    ~Server();
    // `signer` (the MFi chip) is optional; without it /auth-setup errors but
    // pairing still works.
    bool start(uint16_t port, cp_auth_setup::MfiSigner *signer = nullptr);
    void stop();

    // AV capabilities advertised to the phone (screen size, HEVC vs H.264, ...).
    void setAvConfig(const cp_av::Config &cfg) { _avConfig = cfg; }
    // Media sinks forwarded to each connection's AV session (decoded video /
    // audio). Set before start(); unset sinks are simply not called.
    void setAvSinks(const cp_av::Sinks &sinks) { _avSinks = sinks; }
    // Source of outbound input (touch/buttons) forwarded to the AV session.
    // The pointer must outlive the server.
    void setInputSource(cp_av::InputSource *src) { _inputSource = src; }
    // Called (on the accept thread) when a control connection opens and closes,
    // so a backend can track the session lifecycle for its state machine.
    void setLifecycle(std::function<void()> onConnect, std::function<void()> onDisconnect)
    {
        _onConnect = std::move(onConnect);
        _onDisconnect = std::move(onDisconnect);
    }

private:
    void acceptLoop();
    void serveConnection(int clientFd, const struct sockaddr_in6 &peer);

    int _listenFd = -1;
    uint16_t _port = 7000;
    cp_auth_setup::MfiSigner *_signer = nullptr;
    cp_av::Config _avConfig;
    cp_av::Sinks _avSinks;
    cp_av::InputSource *_inputSource = nullptr;
    std::function<void()> _onConnect;
    std::function<void()> _onDisconnect;
    std::atomic<bool> _active{false};
    std::thread _thread;
};
} // namespace cp_server

#endif /* SRC_PROTOCOL_CP_CP_SERVER */
