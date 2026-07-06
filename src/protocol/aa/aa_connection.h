#ifndef SRC_PROTOCOL_AA_AA_CONNECTION
#define SRC_PROTOCOL_AA_AA_CONNECTION

#include <libusb-1.0/libusb.h>

#include <atomic>
#include <thread>
#include <vector>

#include "protocol/iconnection.h"
#include "protocol/usb_buffer.h"
#include "protocol/aa/aa_const.h"
#include "protocol/aa/aa_ssl.h"

// Native wired Android Auto backend: switches the phone into accessory mode
// (AOAP), then speaks the GAL protocol over the bulk endpoints -- version
// handshake, TLS, service discovery and the AV/input/sensor channels. Emits
// Carlinkit-shaped Messages into the IConnection queues so the decoder,
// audio and input layers work unchanged. Thread layout mirrors Connection:
// aa-write (main/reconnect + bulk OUT), aa-read (libusb event pump),
// aa-process (frame reassembly, TLS, dispatch).
class AaConnection : public IConnection
{
public:
    AaConnection();
    virtual ~AaConnection();

    void start() override;
    void stop() override;
    const std::string status() const override;

private:
    struct Context
    {
        AaConnection *owner = nullptr;
        DataSlot *slot = nullptr;
        libusb_transfer *transfer = nullptr;
    };

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

    static void onTransfer(libusb_transfer *transfer);
    void mainLoop();
    void readLoop();
    void processLoop();
    void writeLoop(libusb_device_handle *handler, uint8_t ep);
    libusb_device_handle *waitForAccessory();
    bool link(libusb_device_handle *handler, uint8_t *epIn, uint8_t *epOut);
    void onDeviceConnect(libusb_device_handle *handler, uint8_t endpointIn);
    void onDeviceDisconnect();
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
    // Serialize + encrypt + fragment + bulk-write one message (aa-write thread).
    bool sendFrame(libusb_device_handle *handler, uint8_t ep, uint8_t channel, uint16_t msgId,
                   const uint8_t *body, size_t length, uint8_t flagClass);
    // Translate a Carlinkit-shaped Message from the public writeQueue.
    bool translate(const Message &message, libusb_device_handle *handler, uint8_t ep);
    bool sendKey(libusb_device_handle *handler, uint8_t ep, uint32_t keycode);

    std::thread _writeThread;
    std::thread _readThread;
    std::thread _processThread;

    UsbBuffer _processQueue;
    std::vector<Context> _transfers;
    std::vector<uint8_t> _frameBuffer;
    libusb_context *_context;
    AaSsl _ssl;

    ChannelState _channels[AA_CH_COUNT];

    std::atomic<bool> _active;
    std::atomic<bool> _connected;
    std::atomic<bool> _phoneConnected;
    std::atomic<bool> _auth;
    std::atomic<int64_t> _lastPingSent;
    std::atomic<int64_t> _lastPongReceived;
};

#endif /* SRC_PROTOCOL_AA_AA_CONNECTION */
