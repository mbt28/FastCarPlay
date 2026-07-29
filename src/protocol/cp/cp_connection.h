#ifndef SRC_PROTOCOL_CP_CP_CONNECTION
#define SRC_PROTOCOL_CP_CP_CONNECTION

#ifdef USE_CP_WIRELESS

// Wireless CarPlay backend (protocol = carplay-wireless). Runs the whole
// accessory stack -- the MFi auth chip, the :7000 control server + mDNS advert,
// and the Bluetooth iAP2/CarPlay profiles that make the iPhone offer wireless
// CarPlay and hand off the Wi-Fi credentials -- and feeds the decoded HEVC/H.264
// screen video into videoStream as Carlinkit-shaped CMD_VIDEO_DATA Messages, so
// the app's existing decoder/renderer/display path works unchanged. The daemon
// examples/cp_wireless_serve.cpp does the same assembly headlessly; this wraps it
// behind IConnection. Requires a USE_CP_WIRELESS build (dbus/bluez).

#include <atomic>
#include <memory>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h> // AVCodecID
}

#include "protocol/iconnection.h"

#include "cp_auth_setup.h"
#include "cp_bluetooth.h"
#include "cp_mdns.h"
#include "cp_server.h"
#include "mfi_auth.h"

class CpConnection : public IConnection
{
public:
    CpConnection();
    ~CpConnection() override;

    void start() override;
    void stop() override;
    const std::string status() const override;

    // Wireless CarPlay negotiates HEVC on iOS (we offer it in /info); the config
    // atom confirms the real codec once a frame arrives.
    AVCodecID videoCodec() const override { return _codec.load(); }
    bool videoFocused() const override { return true; }

private:
    void onVideoCodec(bool hevc);
    void onVideo(const std::vector<uint8_t> &annexB);
    void onSessionConnect();
    void onSessionDisconnect();

    MfiAuth _chip;
    bool _haveChip = false;
    std::unique_ptr<cp_auth_setup::MfiSigner> _signer;

    cp_server::Server _server;
    cp_mdns::MdnsResponder _mdns;
    cp_bt::CpBluetooth _bt;

    std::atomic<AVCodecID> _codec{AV_CODEC_ID_HEVC};
    std::atomic<uint32_t> _frames{0};
    std::string _mfiInfo = "no chip";
    bool _started = false;
};

#endif /* USE_CP_WIRELESS */
#endif /* SRC_PROTOCOL_CP_CP_CONNECTION */
