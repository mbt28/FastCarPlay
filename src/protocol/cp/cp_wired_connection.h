#ifndef SRC_PROTOCOL_CP_CP_WIRED_CONNECTION
#define SRC_PROTOCOL_CP_CP_WIRED_CONNECTION

#ifdef USE_CP_WIRED

// Wired CarPlay backend (protocol = carplay-wired). No Wi-Fi, no Bluetooth, no
// ESP32. It brings the USB-plugged iPhone to config 6, opens
// com.apple.carkit.service (the TLS iAP2 control channel) via libimobiledevice
// over our own cp_usbmux daemon, runs the iAP2 identification + MFi auth, and
// hands the phone a CarPlayStartSession pointing at the head unit's link-local
// IPv6 on the kernel USB-NCM interface (usb0). The phone then connects to the
// :7000 CarPlay server over USB and streams the screen -- fed into videoStream
// as Carlinkit-shaped CMD_VIDEO_DATA Messages, exactly like the wireless
// backend, so the app's decoder/renderer/display + touch path work unchanged.
// The daemon examples/cp_wired_serve.cpp does the same assembly headlessly;
// this wraps it behind IConnection. Requires a USE_CP_WIRED build
// (libimobiledevice + libplist).

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h> // AVCodecID
}

#include "protocol/iconnection.h"

#include "cp_auth_setup.h"
#include "cp_av.h"
#include "cp_server.h"
#include "cp_usbmux.h"
#include "mfi_auth.h"

class Message;

// Also a cp_av::InputSource: the writer thread translates the app's writeQueue
// (CMD_TOUCH / CMD_MULTI_TOUCH / CMD_CONTROL) into CarPlay event-channel
// commands the AV session pulls and POSTs to the phone (shared with the wireless
// backend's semantics).
class CpWiredConnection : public IConnection, public cp_av::InputSource
{
public:
    CpWiredConnection();
    ~CpWiredConnection() override;

    void start() override;
    void stop() override;
    const std::string status() const override;

    AVCodecID videoCodec() const override { return _codec.load(); }
    bool videoFocused() const override { return true; }

    bool nextCommand(std::vector<uint8_t> &body) override;

private:
    // ── AV + input glue (mirrors CpConnection) ───────────────────────────
    void onVideoCodec(bool hevc);
    void onVideo(const std::vector<uint8_t> &annexB);
    void onAudio(int type, int rate, int channels, const std::vector<uint8_t> &pcm);
    void onSessionConnect();
    void onSessionDisconnect();
    void writerLoop();
    void handleInput(const Message &m);
    void enqueueCommand(std::vector<uint8_t> body);
    void clearInput();

    // ── The wired trigger ────────────────────────────────────────────────
    void triggerLoop();      // reconnect loop: wait for phone -> run a session
    bool runWiredSession();  // one attempt: usbmux + carkit + iAP2 + StartSession

    MfiAuth _chip;
    bool _haveChip = false;
    int _protoMajor = 0;
    std::unique_ptr<cp_auth_setup::MfiSigner> _signer;
    std::vector<uint8_t> _mfiCert;

    cp_server::Server _server;
    cp_usbmux::Usbmux _mux;
    std::string _pk;       // accessory public key (hex) for CarPlayStartSession
    std::string _deviceId; // accessory device id (MAC-format) for the handoff

    std::thread _writer, _trigger;
    std::atomic<bool> _active{false};
    std::mutex _inputMutex;
    std::deque<std::vector<uint8_t>> _inputQueue;

    std::atomic<AVCodecID> _codec{AV_CODEC_ID_HEVC};
    std::atomic<uint32_t> _frames{0};
    std::string _mfiInfo = "no chip";
    bool _started = false;
};

#endif /* USE_CP_WIRED */
#endif /* SRC_PROTOCOL_CP_CP_WIRED_CONNECTION */
