#include "aa_tcp_transport.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.h"

#define AA_TCP_ACCEPT_POLL 200 // ms per accept poll (so shutdown is prompt)

AaTcpTransport::AaTcpTransport(uint16_t port)
    : _port(port), _listenFd(-1), _clientFd(-1), _connected(false)
{
}

AaTcpTransport::~AaTcpTransport()
{
    close();
    if (_listenFd >= 0)
    {
        ::close(_listenFd);
        _listenFd = -1;
    }
}

bool AaTcpTransport::ensureListen()
{
    if (_listenFd >= 0)
        return true;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        log_e("TCP: can't create socket > %s", strerror(errno));
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(_port);
    if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        log_e("TCP: can't bind port %d > %s", _port, strerror(errno));
        ::close(fd);
        return false;
    }
    if (::listen(fd, 1) < 0)
    {
        log_e("TCP: can't listen > %s", strerror(errno));
        ::close(fd);
        return false;
    }

    _listenFd = fd;
    log_i("TCP: listening on port %d", _port);
    return true;
}

bool AaTcpTransport::open(std::atomic<bool> &active)
{
    if (!ensureListen())
        return false;

    while (active.load())
    {
        struct pollfd pfd{_listenFd, POLLIN, 0};
        int r = poll(&pfd, 1, AA_TCP_ACCEPT_POLL);
        if (r <= 0)
            continue; // timeout / EINTR -> re-check active
        if (!(pfd.revents & POLLIN))
            continue;

        sockaddr_in peer = {};
        socklen_t plen = sizeof(peer);
        int fd = ::accept(_listenFd, (sockaddr *)&peer, &plen);
        if (fd < 0)
            continue;

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        _clientFd = fd;
        _connected = true;
        log_i("TCP: phone connected from %s", inet_ntoa(peer.sin_addr));
        return true;
    }
    return false;
}

void AaTcpTransport::close()
{
    _connected = false;
    if (_clientFd >= 0)
    {
        ::shutdown(_clientFd, SHUT_RDWR); // unblock any pending read/write
        ::close(_clientFd);
        _clientFd = -1;
    }
}

bool AaTcpTransport::read(uint8_t *dst, uint32_t len)
{
    uint32_t got = 0;
    while (got < len && _connected)
    {
        ssize_t n = ::recv(_clientFd, dst + got, len - got, 0);
        if (n > 0)
        {
            got += (uint32_t)n;
            continue;
        }
        if (n == 0 || (errno != EINTR && errno != EAGAIN))
        {
            _connected = false;
            return false;
        }
    }
    return got == len;
}

bool AaTcpTransport::write(const uint8_t *src, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::send(_clientFd, src + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0)
        {
            sent += (uint32_t)n;
            continue;
        }
        if (errno == EINTR || errno == EAGAIN)
            continue;
        log_w("TCP: write failed > %s", strerror(errno));
        _connected = false;
        return false;
    }
    return true;
}
