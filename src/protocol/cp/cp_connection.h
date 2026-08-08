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
#include "protocol/wifi_ap.h"

#include "cp_auth_setup.h"
#include "cp_av.h"
#include "cp_bluetooth.h"
#include "cp_mdns.h"
#include "cp_server.h"
#include "mfi_auth.h"

class Message;

// Also a cp_av::InputSource: the writer thread translates the app's writeQueue
// (CMD_TOUCH / CMD_MULTI_TOUCH / CMD_CONTROL) into CarPlay event-channel commands
// (HID reports etc.) that the AV session pulls and POSTs to the phone.
class CpConnection : public IConnection, public cp_av::InputSource
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
    // The user can hand the screen back to FastCarPlay (video-release button) and
    // resume (video-focus button / the home Resume row) without dropping the
    // CarPlay session -- same UX as Android Auto.
    bool videoFocused() const override { return _videoFocused.load(); }
    void requestVideoFocus() override;

    // cp_av::InputSource: pop the next queued event-channel command body.
    bool nextCommand(std::vector<uint8_t> &body) override;

private:
    void onVideoCodec(bool hevc);
    void onVideo(const std::vector<uint8_t> &annexB);
    void onAudio(int type, int rate, int channels, const std::vector<uint8_t> &pcm);
    void onSessionConnect();
    void onSessionDisconnect();

    void writerLoop();
    void handleInput(const Message &m);
    void enqueueCommand(std::vector<uint8_t> body);
    void clearInput();

    MfiAuth _chip;
    bool _haveChip = false;
    std::unique_ptr<cp_auth_setup::MfiSigner> _signer;

    WifiAp _wifi;
    cp_server::Server _server;
    // The screen advertised to the phone in /info. Touch coordinates must be in
    // this same pixel space, so both readers take it from here.
    cp_av::Config _avcfg;
    cp_mdns::MdnsResponder _mdns;
    cp_bt::CpBluetooth _bt;

    std::thread _writer;
    std::atomic<bool> _active{false};
    std::mutex _inputMutex;
    std::deque<std::vector<uint8_t>> _inputQueue; // outbound event-command bodies

    std::atomic<AVCodecID> _codec{AV_CODEC_ID_HEVC};
    std::atomic<bool> _videoFocused{true}; // false = handed back to FastCarPlay
    std::atomic<uint32_t> _frames{0};
    std::string _mfiInfo = "no chip";
    bool _started = false;
};

#endif /* USE_CP_WIRELESS */
#endif /* SRC_PROTOCOL_CP_CP_CONNECTION */
