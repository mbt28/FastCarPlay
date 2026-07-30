#ifndef SRC_PROTOCOL_CP_CP_USBMUX
#define SRC_PROTOCOL_CP_CP_USBMUX

// A self-contained usbmux daemon for wired CarPlay -- the C++ replacement for
// LIVI's muxd.py. It brings a plugged iPhone to USB config 6 (the CarPlay USB
// mode: vendor request 0xC0/0x52 to reveal the hidden configs, then select
// config 6 via sysfs), claims the config-6 usbmux interface (iface 1) over
// usbfs, speaks Apple's usbmux mux-TCP protocol on its bulk endpoints, and
// exposes a usbmuxd-compatible UNIX socket. libimobiledevice then reaches
// lockdown + com.apple.carkit.service over it via
// USBMUXD_SOCKET_ADDRESS=UNIX:<socketPath()> -- the TLS iAP2 channel the wired
// CarPlay trigger runs on. It only claims iface 1; the CDC-NCM interfaces are
// left to the kernel cdc_ncm driver (which brings up usb0 for the AV link).
//
// Serves ListDevices / ReadBUID / Listen / ReadPairRecord / SavePairRecord /
// Connect from /var/lib/lockdown, mirroring muxd.py. Root required (usbfs +
// sysfs config switch). XML plists via libplist.

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace cp_usbmux
{
class MuxHost;   // mux-TCP over the usbmux bulk endpoints
class MuxServer; // usbmuxd-compatible UNIX socket

class Usbmux
{
public:
    Usbmux();
    ~Usbmux();

    // Bring the Apple device with this sysfs serial (24-char, non-dashed) to
    // config 6, claim the usbmux interface, and start the UNIX-socket server.
    // Returns false if the phone is absent, won't expose config 6, or the mux
    // handshake fails. On success socketPath() is ready for libimobiledevice.
    bool start(const std::string &serial);
    void stop();

    bool running() const { return _running.load(); }
    const std::string &socketPath() const { return _socketPath; }
    const std::string &serial() const { return _serial; }

private:
    bool configCarplay(); // reveal + select config 6

    std::string _serial;
    std::string _socketPath;
    std::atomic<bool> _running{false};
    std::unique_ptr<MuxHost> _host;
    std::unique_ptr<MuxServer> _server;
};
} // namespace cp_usbmux

#endif /* SRC_PROTOCOL_CP_CP_USBMUX */
