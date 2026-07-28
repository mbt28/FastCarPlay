#include "cp_server.h"

#include <cstring>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "cp_control_channel.h"
#include "common/logger.h"

namespace cp_server
{
Server::~Server() { stop(); }

bool Server::start(uint16_t port, cp_auth_setup::MfiSigner *signer)
{
    _port = port;
    _signer = signer;

    _listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listenFd < 0)
        return false;
    int yes = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(_listenFd, 1) != 0)
    {
        log_e("cp-server: bind/listen :%d failed", port);
        close(_listenFd);
        _listenFd = -1;
        return false;
    }

    _active = true;
    _thread = std::thread(&Server::acceptLoop, this);
    log_i("cp-server: listening on :%d", port);
    return true;
}

void Server::acceptLoop()
{
    while (_active)
    {
        struct pollfd pfd{_listenFd, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0)
            continue;

        struct sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int client = accept(_listenFd, (struct sockaddr *)&from, &fromLen);
        if (client < 0)
            continue;
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        log_i("cp-server: connection from %s", inet_ntoa(from.sin_addr));
        serveConnection(client); // one control connection at a time (single session)
        close(client);
        log_v("cp-server: connection closed");
    }
}

void Server::serveConnection(int clientFd)
{
    cp_control_channel::ControlChannel channel(_signer);
    uint8_t buf[8192];

    while (_active)
    {
        struct pollfd pfd{clientFd, POLLIN, 0};
        const int r = poll(&pfd, 1, 500);
        if (r == 0)
            continue;
        if (r < 0 || (pfd.revents & (POLLERR | POLLHUP)))
            break;

        ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0)
            break; // peer closed

        std::vector<uint8_t> out = channel.process(std::vector<uint8_t>(buf, buf + n));
        size_t off = 0;
        while (off < out.size())
        {
            ssize_t w = send(clientFd, out.data() + off, out.size() - off, 0);
            if (w <= 0)
                return;
            off += (size_t)w;
        }
    }
}

void Server::stop()
{
    if (!_active && _listenFd < 0)
        return;
    _active = false;
    if (_thread.joinable())
        _thread.join();
    if (_listenFd >= 0)
    {
        close(_listenFd);
        _listenFd = -1;
    }
}
} // namespace cp_server
