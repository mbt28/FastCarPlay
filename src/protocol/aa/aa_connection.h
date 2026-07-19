#ifndef SRC_PROTOCOL_AA_AA_CONNECTION
#define SRC_PROTOCOL_AA_AA_CONNECTION

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "protocol/iconnection.h"
#include "protocol/aa/aa_const.h"
#include "protocol/aa/aa_ssl.h"
#include "protocol/aa/aa_transport.h"

// Native Android Auto backend: speaks the GAL protocol (version handshake,
// TLS, service discovery, AV/input/sensor channels) over an AaTransport, which
// is USB/AOAP for wired and a TCP socket for wireless. Emits Carlinkit-shaped
// Messages into the IConnection queues so the decoder, audio and input layers
// work unchanged. Threads: aa-main (reconnect + write), aa-process (frame
// reassembly, TLS, dispatch); the transport owns any read pipeline it needs.
class AaConnection : public IConnection
{
public:
    // Takes the byte transport under the session (defaults to USB/AOAP).
    explicit AaConnection(std::unique_ptr<AaTransport> transport = nullptr);
    virtual ~AaConnection();

    void start() override;
    void stop() override;
    const std::string status() const override;

    bool videoFocused() const override { return _videoFocused; }
    void requestVideoFocus() override;

private:
    struct ChannelState
    {
        bool open = false;
        bool assembling = false;
        int32_t session = -1;
        std::vector<uint8_t> assembly;

        void reset()
        {
            open = false;
            assembling = false;
            session = -1;
            assembly.clear();
            assembly.shrink_to_fit();
        }
    };

    void mainLoop();
    void processLoop();
    void writeLoop();
    void onConnect();
    void onDisconnect();
    void onPhoneConnect();
    void onPhoneDisconnect();

    // Frame dispatch (aa-process thread)
    void handleMessage(uint8_t channel, const uint8_t *data, size_t length);
    void handleControl(uint16_t msgId, const uint8_t *data, size_t length);
    void handleMedia(uint8_t channel, uint16_t msgId, const uint8_t *data, size_t length);
    void handleSensor(uint16_t msgId, const uint8_t *data, size_t length);
    void handleInput(uint16_t msgId, const uint8_t *data, size_t length);
    void emitVideo(const uint8_t *data, size_t length);
    void emitAudio(uint8_t channel, const uint8_t *data, size_t length);

    // Queue a protocol-generated message for the write thread. flagClass is
    // one of AA_FLAG_PLAINTEXT / AA_FLAG_ENC_SIGNAL / AA_FLAG_ENC_CONTROL.
    void queueFrame(uint8_t channel, uint16_t msgId, const std::vector<uint8_t> &body,
                    uint8_t flagClass = AA_FLAG_ENC_SIGNAL);
    // Serialize + encrypt + fragment + write one message over the transport.
    bool sendFrame(uint8_t channel, uint16_t msgId, const uint8_t *body, size_t length,
                   uint8_t flagClass);
    // Translate a Carlinkit-shaped Message from the public writeQueue.
    bool translate(const Message &message);
    bool sendKey(uint32_t keycode);

    std::unique_ptr<AaTransport> _transport;

    std::thread _writeThread;
    std::thread _processThread;

    std::vector<uint8_t> _frameBuffer;
    AaSsl _ssl;

    ChannelState _channels[AA_CH_COUNT];

    std::atomic<bool> _videoFocused{true};
    std::atomic<bool> _active;
    std::atomic<bool> _phoneConnected;
    std::atomic<bool> _auth;
    std::atomic<int64_t> _lastPingSent;
    std::atomic<int64_t> _lastPongReceived;
};

#endif /* SRC_PROTOCOL_AA_AA_CONNECTION */
