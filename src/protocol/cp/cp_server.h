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
#include <thread>

#include "cp_auth_setup.h"

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

private:
    void acceptLoop();
    void serveConnection(int clientFd);

    int _listenFd = -1;
    uint16_t _port = 7000;
    cp_auth_setup::MfiSigner *_signer = nullptr;
    std::atomic<bool> _active{false};
    std::thread _thread;
};
} // namespace cp_server

#endif /* SRC_PROTOCOL_CP_CP_SERVER */
