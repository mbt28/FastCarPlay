// cp_usbmux_serve -- run the C++ usbmux daemon (cp_usbmux) standalone, the
// self-contained replacement for LIVI's Python muxd. Brings the phone to config
// 6, claims the usbmux interface, and serves a usbmuxd-compatible socket. Test:
//   sudo ./cp_usbmux_serve 00008150001C31300A63401C
//   USBMUXD_SOCKET_ADDRESS=UNIX:/tmp/fcp-usbmux-00008150.sock ideviceinfo -u <dashed>
#include <csignal>
#include <cstdio>
#include <unistd.h>

#include "common/logger.h"
#include "protocol/cp/cp_usbmux.h"

static volatile sig_atomic_t g_quit = 0;
static void onSignal(int) { g_quit = 1; }

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: %s <sysfs-serial (24-char, non-dashed)>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *lvl = getenv("FCP_LOG");
    set_log_level(lvl ? atoi(lvl) : (int)Logger::Level::Debug);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    cp_usbmux::Usbmux mux;
    if (!mux.start(argv[1]))
    {
        printf("cp_usbmux start failed\n");
        return 1;
    }
    printf("SOCKET=%s\n", mux.socketPath().c_str());
    printf("Ready. Ctrl-C to stop.\n");
    while (!g_quit)
        pause();
    printf("stopping\n");
    mux.stop();
    return 0;
}
