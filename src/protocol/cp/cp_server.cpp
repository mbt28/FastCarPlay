#include "cp_server.h"

#include <cerrno>
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

    // Dual-stack IPv6: wireless CarPlay connects to the accessory's Wi-Fi IPv6
    // link-local (fe80::), while the wired/desktop paths use IPv4. An
    // AF_INET6 socket with IPV6_V6ONLY off accepts both (IPv4 as v4-mapped).
    _listenFd = socket(AF_INET6, SOCK_STREAM, 0);
    if (_listenFd < 0)
    {
        // Most likely a kernel built without IPv6 (EAFNOSUPPORT). Worth saying
        // plainly: CarPlay cannot work without it -- the handoff hands the phone
        // an fe80:: link-local to connect back to -- and the caller would
        // otherwise report this as "already running?".
        log_e("cp-server: cannot create an IPv6 socket (%s). CarPlay needs IPv6; "
              "check CONFIG_IPV6 in the kernel (/proc/net/if_inet6 should exist)",
              strerror(errno));
        return false;
    }
    int yes = 1;
    setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int no = 0;
    setsockopt(_listenFd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(_listenFd, 1) != 0)
    {
        log_e("cp-server: bind/listen :%d failed (%s)", port, strerror(errno));
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

        struct sockaddr_storage from{};
        socklen_t fromLen = sizeof(from);
        int client = accept(_listenFd, (struct sockaddr *)&from, &fromLen);
        if (client < 0)
            continue;
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        char host[INET6_ADDRSTRLEN] = "?";
        struct sockaddr_in6 peer{};
        if (from.ss_family == AF_INET6)
        {
            peer = *(struct sockaddr_in6 *)&from;
            inet_ntop(AF_INET6, &peer.sin6_addr, host, sizeof(host));
        }
        else
            inet_ntop(AF_INET, &((struct sockaddr_in *)&from)->sin_addr, host, sizeof(host));
        log_i("cp-server: connection from %s", host);
        serveConnection(client, peer); // one control connection at a time (single session)
        close(client);
        log_v("cp-server: connection closed");
    }
}

void Server::serveConnection(int clientFd, const struct sockaddr_in6 &peer)
{
    cp_control_channel::ControlChannel channel(_signer);
    channel.setPeer(peer);
    channel.setAvConfig(_avConfig);
    channel.setAvSinks(_avSinks);
    channel.setInputSource(_inputSource);
    uint8_t buf[8192];

    if (_onConnect)
        _onConnect();

    bool sendFailed = false;
    while (_active && !sendFailed)
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
            {
                sendFailed = true;
                break;
            }
            off += (size_t)w;
        }
    }

    if (_onDisconnect)
        _onDisconnect();
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
