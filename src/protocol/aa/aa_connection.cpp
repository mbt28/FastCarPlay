#include "aa_connection.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "libavcodec/avcodec.h"

#include "protocol/aa/aoap.h"
#include "protocol/aa/aa_proto.h"
#include "protocol/message.h"
#include "protocol/protocol_const.h"
#include "common/functions.h"
#include "common/logger.h"
#include "common/threading.h"
#include "settings.h"

#define AA_PING_TIMEOUT 10000 // ms without a pong before the link is dropped

static int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static uint64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void writeU32le(uint8_t *dst, uint32_t value)
{
    dst[0] = value & 0xFF;
    dst[1] = (value >> 8) & 0xFF;
    dst[2] = (value >> 16) & 0xFF;
    dst[3] = (value >> 24) & 0xFF;
}

static uint32_t readU32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Pixel dimensions of the advertised video/touch space (aa-resolution 1/2/3).
static void aaVideoSize(int &width, int &height)
{
    switch (Settings::aaResolution)
    {
    case 3:
        width = 1920;
        height = 1080;
        break;
    case 2:
        width = 1280;
        height = 720;
        break;
    default:
        width = 800;
        height = 480;
        break;
    }
}

AaConnection::AaConnection()
    : _processQueue(Settings::usbBuffer, Settings::usbTransferSize),
      _transfers(Settings::usbQueue),
      _context(nullptr),
      _active(false),
      _connected(false),
      _phoneConnected(false),
      _auth(false),
      _lastPingSent(0),
      _lastPongReceived(0)
{
    int result = libusb_init(&_context);
    if (result < 0)
        throw std::runtime_error(std::string("Can't initialise USB: ") + libusb_error_name(result));

    for (Context &context : _transfers)
    {
        context.owner = this;
        context.transfer = nullptr;
        context.slot = nullptr;
    }

    _method = "android-auto-usb";
    log_v("Created");
}

AaConnection::~AaConnection()
{
    log_v("Destroying");
    stop();

    for (Context &context : _transfers)
    {
        if (context.transfer)
        {
            libusb_free_transfer(context.transfer);
            context.transfer = nullptr;
        }
    }

    if (_context)
    {
        libusb_exit(_context);
        _context = nullptr;
    }
    log_v("Destroyed");
}

void AaConnection::start()
{
    if (_active)
        return;

    _state = PROTOCOL_STATUS_INITIALISING;
    log_v("Starting");

    for (Context &context : _transfers)
    {
        if (!context.transfer)
        {
            context.transfer = libusb_alloc_transfer(0);
            if (context.transfer == nullptr)
            {
                log_e("Can't allocate usb transfer");
                return;
            }
        }
    }

    _active = true;
    _writeThread = std::thread(&AaConnection::mainLoop, this);
}

void AaConnection::stop()
{
    if (!_active)
        return;

    log_v("Stopping");

    _active = false;
    _connected = false;
    _state = PROTOCOL_STATUS_UNKNOWN;

    _processQueue.notify();
    writeQueue.notify();

    if (_writeThread.joinable())
        _writeThread.join();

    log_v("Stopped");
}

const std::string AaConnection::status() const
{
    std::stringstream result;
    result << "aa-usb " << _ssl.cipherName();
    for (int i = 0; i < AA_CH_COUNT; i++)
        if (_channels[i].open)
            result << " ch" << i;
    return result.str();
}

void AaConnection::onTransfer(libusb_transfer *transfer)
{
    if (!transfer || !transfer->user_data)
        return;

    Context *c = static_cast<Context *>(transfer->user_data);
    if (!c->owner->_connected)
        return;

    c->owner->_transfered.fetch_add(transfer->actual_length, std::memory_order_relaxed);
    log_p("Transfer %d [%d] > %s", transfer->actual_length, transfer->status,
          bytes(transfer->buffer, transfer->actual_length, 40).c_str());

    if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
        return;

    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
    {
        c->owner->_connected = false;
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        c->slot->commit(transfer->actual_length);
        c->slot = c->owner->_processQueue.get();
        if (!c->slot)
        {
            log_e("Can't allocate data slot for next usb transfer, increase usb buffer slots");
            c->owner->_connected = false;
            return;
        }
        c->transfer->buffer = c->slot->data;
    }
    int status = libusb_submit_transfer(c->transfer);
    if (status != LIBUSB_SUCCESS)
    {
        log_w("USB transfer re-submit failed with status %d", status);
        c->owner->_connected = false;
    }
}

// Wait for a phone to show up in accessory mode; kick the AOAP switch on
// anything that looks like a candidate while waiting.
libusb_device_handle *AaConnection::waitForAccessory()
{
    libusb_device_handle *handler = aoap::openAccessory(_context);
    if (handler)
        return handler;

    if (aoap::switchCandidates(_context) == 0)
        return nullptr;

    // Devices re-enumerate within a few hundred ms; the first connection can
    // take longer because the phone shows a consent dialog.
    int64_t deadline = nowMs() + Settings::aaAccessoryTimeout;
    while (_active && nowMs() < deadline)
    {
        writeQueue.waitFor(_active, AA_RECONNECT_TIMEOUT);
        handler = aoap::openAccessory(_context);
        if (handler)
            return handler;
    }
    return nullptr;
}

void AaConnection::mainLoop()
{
    setThreadName("aa-write");
    log_d("AA writing thread started");

    int connectCount = 0;

    while (_active)
    {
        libusb_device_handle *handler = waitForAccessory();
        if (handler)
        {
            connectCount = 0;
            _state = PROTOCOL_STATUS_LINKING;

            uint8_t endpointIn = 0;
            uint8_t endpointOut = 0;
            char error[256] = {0};

            if (link(handler, &endpointIn, &endpointOut))
            {
                if (_ssl.init(error))
                {
                    onDeviceConnect(handler, endpointIn);
                    writeLoop(handler, endpointOut);
                    onDeviceDisconnect();
                }
                else
                {
                    log_e("SSL init failed > %s", error);
                }
                _state = PROTOCOL_STATUS_ERROR;
            }

            libusb_release_interface(handler, 0);
            // Reset the device before dropping it: this kicks the phone out of
            // accessory mode so the next iteration performs a fresh AOAP switch
            // and gets a clean accessory session, rather than reconnecting to a
            // wedged/half-open pipe (which NAKs every write -> ERROR_TIMEOUT).
            libusb_reset_device(handler);
            libusb_close(handler);
        }
        else if (_state != PROTOCOL_STATUS_NO_DEVICE && connectCount++ > AA_CONNECT_RETRY)
        {
            _state = PROTOCOL_STATUS_NO_DEVICE;
        }
        writeQueue.waitFor(_active, AA_RECONNECT_TIMEOUT);
    }

    log_v("AA writing thread stopped");
}

bool AaConnection::link(libusb_device_handle *handler, uint8_t *epIn, uint8_t *epOut)
{
    // Neither libusb_reset_device nor an unconditional set_configuration here:
    // both drop the phone out of accessory mode. libusb_set_configuration on a
    // device that already has the target configuration acts as a lightweight
    // *device reset*, which makes an accessory-mode phone re-enumerate in a
    // loop. Only set the configuration if it isn't already active.
    libusb_set_auto_detach_kernel_driver(handler, 1);
    int currentConfig = 0;
    if (libusb_get_configuration(handler, &currentConfig) == LIBUSB_SUCCESS && currentConfig != 1)
    {
        int result = libusb_set_configuration(handler, 1);
        if (result != LIBUSB_SUCCESS)
            log_v("set_configuration: %s (continuing)", libusb_error_name(result));
    }

    int result = libusb_claim_interface(handler, 0);
    if (result != LIBUSB_SUCCESS)
    {
        log_w("Can't claim interface > %s", libusb_error_name(result));
        return false;
    }

    libusb_device *device = libusb_get_device(handler);
    struct libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS)
    {
        log_w("Can't get config descriptor");
        return false;
    }

    *epIn = 0;
    *epOut = 0;
    for (int i = 0; i < config->interface[0].altsetting[0].bNumEndpoints; i++)
    {
        const struct libusb_endpoint_descriptor *ep = &config->interface[0].altsetting[0].endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
            continue;
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN)
            *epIn = ep->bEndpointAddress;
        else
            *epOut = ep->bEndpointAddress;
    }
    libusb_free_config_descriptor(config);

    if (!*epIn || !*epOut)
    {
        log_w("Accessory device has no bulk endpoint pair");
        return false;
    }

    log_i("Accessory linked %d:%d speed: %d", libusb_get_bus_number(device),
          libusb_get_device_address(device), libusb_get_device_speed(device));
    return true;
}

void AaConnection::onDeviceConnect(libusb_device_handle *handler, uint8_t endpointIn)
{
    _connected = true;
    _phoneConnected = false;
    _auth = false;
    _lastPingSent = 0;
    _lastPongReceived = 0;
    for (ChannelState &channel : _channels)
        channel.reset();

    writeQueue.clear();
    videoStream.clear();
    audioStreamMain.clear();
    audioStreamAux.clear();
    _processQueue.reset();

    _processThread = std::thread(&AaConnection::processLoop, this);
    _readThread = std::thread(&AaConnection::readLoop, this);

    for (Context &context : _transfers)
    {
        context.owner = this;
        context.slot = _processQueue.get();
        if (context.slot == nullptr)
        {
            log_e("Can't allocate data slot for usb transfer, increase usb buffer slots");
            _connected = false;
            return;
        }
        libusb_fill_bulk_transfer(context.transfer, handler, endpointIn, context.slot->data,
                                  context.slot->size, AaConnection::onTransfer, &context, 0);
        int status = libusb_submit_transfer(context.transfer);
        if (status != LIBUSB_SUCCESS)
        {
            log_w("USB transfer submit failed with code %d", status);
            _connected = false;
            return;
        }
    }

    // Open the conversation: version request, raw u16BE major/minor pair.
    queueFrame(AA_CH_CONTROL, AA_MSG_VERSION_REQUEST,
               {0, AA_VERSION_MAJOR, 0, AA_VERSION_MINOR}, AA_FLAG_PLAINTEXT);
}

void AaConnection::onDeviceDisconnect()
{
    onPhoneDisconnect();

    log_i("Device disconnected");
    _connected = false;
    _auth = false;
    _processQueue.notify();

    if (_readThread.joinable())
        _readThread.join();

    if (_processThread.joinable())
        _processThread.join();

    _ssl.reset();
    for (ChannelState &channel : _channels)
        channel.reset();
}

void AaConnection::onPhoneConnect()
{
    if (_phoneConnected)
        return;
    _state = PROTOCOL_STATUS_CONNECTED;
    log_i("Phone connected (Android Auto)");
    _phoneConnected = true;

    if (Settings::onConnect.value.length() > 1)
        execute(Settings::onConnect.value.c_str());
}

void AaConnection::onPhoneDisconnect()
{
    if (!_phoneConnected)
        return;
    _state = PROTOCOL_STATUS_ONLINE;
    log_i("Phone disconnected");
    _phoneConnected = false;

    if (Settings::onDisconnect.value.length() > 1)
        execute(Settings::onDisconnect.value.c_str());

    _phoneName = "phone";
}

void AaConnection::readLoop()
{
    setThreadName("aa-read");
    setThreadPriority(ThreadPriority::Realtime);
    timeval timeout{0, 1000};

    log_d("AA reading thread started");

    while (_connected)
    {
        libusb_handle_events_timeout_completed(_context, &timeout, nullptr);
    }

    log_v("Canceling transfer requests");

    for (Context &context : _transfers)
    {
        if (context.transfer)
            libusb_cancel_transfer(context.transfer);
        libusb_handle_events_timeout_completed(_context, &timeout, nullptr);
    }

    log_v("AA reading thread stopped");
}

void AaConnection::processLoop()
{
    setThreadName("aa-process");
    log_d("AA processing thread started");

    while (_connected)
    {
        // Frame header: channel, flags, u16BE payload size. FIRST-without-
        // LAST frames add a u32BE total size for the reassembled message.
        uint8_t head[4];
        if (!_processQueue.read(head, sizeof(head), _connected))
            break;

        uint8_t channelId = head[0];
        uint8_t flags = head[1];
        uint32_t payloadSize = (head[2] << 8) | head[3];
        bool first = flags & AA_FRAME_FIRST;
        bool last = flags & AA_FRAME_LAST;
        bool encrypted = flags & AA_FRAME_ENCRYPTED;

        // AA framing has no magic to resync on -- treat anomalies as fatal.
        if (channelId >= AA_CH_COUNT || (flags & ~0x0F) != 0)
        {
            log_w("Frame desync (channel %d flags %02x), reconnecting", channelId, flags);
            _connected = false;
            break;
        }

        uint32_t totalSize = 0;
        if (first && !last)
        {
            uint8_t ext[4];
            if (!_processQueue.read(ext, sizeof(ext), _connected))
                break;
            totalSize = (ext[0] << 24) | (ext[1] << 16) | (ext[2] << 8) | ext[3];
            if (totalSize > AA_MAX_MESSAGE_SIZE)
            {
                log_w("Frame desync (total size %u), reconnecting", totalSize);
                _connected = false;
                break;
            }
        }

        _frameBuffer.resize(payloadSize);
        if (payloadSize > 0 && !_processQueue.read(_frameBuffer.data(), payloadSize, _connected))
            break;

        ChannelState &channel = _channels[channelId];
        if (first)
        {
            channel.assembly.clear();
            if (totalSize)
                channel.assembly.reserve(totalSize);
            channel.assembling = true;
        }
        else if (!channel.assembling)
        {
            log_w("Continuation frame without start on channel %d, dropped", channelId);
            continue;
        }

        if (encrypted)
        {
            char error[256] = {0};
            if (!_ssl.decrypt(_frameBuffer.data(), payloadSize, channel.assembly, error))
            {
                log_w("TLS decrypt failed > %s", error);
                _connected = false;
                break;
            }
        }
        else
        {
            channel.assembly.insert(channel.assembly.end(), _frameBuffer.begin(),
                                    _frameBuffer.end());
        }

        if (channel.assembly.size() > AA_MAX_MESSAGE_SIZE)
        {
            log_w("Message overflow on channel %d, reconnecting", channelId);
            _connected = false;
            break;
        }

        if (last)
        {
            channel.assembling = false;
            handleMessage(channelId, channel.assembly.data(), channel.assembly.size());
            channel.assembly.clear();
        }
    }

    log_v("AA processing thread stopped");
}

void AaConnection::handleMessage(uint8_t channel, const uint8_t *data, size_t length)
{
    if (length < 2)
    {
        log_w("Short message on channel %d (%zu bytes)", channel, length);
        return;
    }

    uint16_t msgId = (data[0] << 8) | data[1];
    const uint8_t *body = data + 2;
    size_t bodyLength = length - 2;

    log_d("RX ch %d id 0x%04x (%zu bytes)", channel, msgId, bodyLength);

    // Channel open requests arrive on the channel being opened.
    if (msgId == AA_MSG_CHANNEL_OPEN_REQUEST && channel != AA_CH_CONTROL)
    {
        int32_t serviceId = -1;
        aa_proto::parseChannelOpenRequest(body, bodyLength, serviceId);
        log_i("Channel %d open request (service %d)", channel, serviceId);
        _channels[channel].open = true;
        queueFrame(channel, AA_MSG_CHANNEL_OPEN_RESPONSE, aa_proto::channelOpenResponse(),
                   AA_FLAG_ENC_CONTROL);
        if (channel == AA_CH_SENSOR)
        {
            // The phone blocks projection until it knows the driving status.
            queueFrame(AA_CH_SENSOR, AA_MSG_SENSOR_BATCH, aa_proto::sensorBatchDrivingStatus(0));
        }
        return;
    }

    switch (channel)
    {
    case AA_CH_CONTROL:
        handleControl(msgId, body, bodyLength);
        break;
    case AA_CH_VIDEO:
    case AA_CH_MEDIA_AUDIO:
    case AA_CH_SPEECH_AUDIO:
    case AA_CH_SYSTEM_AUDIO:
        handleMedia(channel, msgId, body, bodyLength);
        break;
    case AA_CH_SENSOR:
        handleSensor(msgId, body, bodyLength);
        break;
    case AA_CH_INPUT:
        handleInput(msgId, body, bodyLength);
        break;
    default:
        log_d("Message 0x%04x on unhandled channel %d", msgId, channel);
        break;
    }
}

void AaConnection::handleControl(uint16_t msgId, const uint8_t *data, size_t length)
{
    switch (msgId)
    {
    case AA_MSG_VERSION_RESPONSE:
    {
        // Raw: u16BE major, minor, status.
        uint16_t status = length >= 6 ? (data[4] << 8) | data[5] : 0xffff;
        if (status != AA_VERSION_STATUS_MATCH)
        {
            log_e("Version mismatch (phone %d.%d status %d)", length >= 2 ? (data[0] << 8) | data[1] : 0,
                  length >= 4 ? (data[2] << 8) | data[3] : 0, status);
            _connected = false;
            return;
        }
        log_i("Version handshake done (%d.%d), starting TLS", (data[0] << 8) | data[1],
              (data[2] << 8) | data[3]);
        std::vector<uint8_t> out;
        bool done = false;
        char error[256] = {0};
        if (!_ssl.handshake(nullptr, 0, out, done, error))
        {
            log_e("TLS start failed > %s", error);
            _connected = false;
            return;
        }
        if (!out.empty())
            queueFrame(AA_CH_CONTROL, AA_MSG_SSL_HANDSHAKE, out, AA_FLAG_PLAINTEXT);
        break;
    }

    case AA_MSG_SSL_HANDSHAKE:
    {
        std::vector<uint8_t> out;
        bool done = false;
        char error[256] = {0};
        if (!_ssl.handshake(data, length, out, done, error))
        {
            log_e("TLS handshake failed > %s", error);
            _connected = false;
            return;
        }
        if (!out.empty())
            queueFrame(AA_CH_CONTROL, AA_MSG_SSL_HANDSHAKE, out, AA_FLAG_PLAINTEXT);
        if (done)
        {
            log_i("TLS established (%s)", _ssl.cipherName().c_str());
            queueFrame(AA_CH_CONTROL, AA_MSG_AUTH_COMPLETE, aa_proto::authComplete(), AA_FLAG_PLAINTEXT);
            _auth = true;
            _lastPongReceived = nowMs();
        }
        break;
    }

    case AA_MSG_SERVICE_DISCOVERY_REQUEST:
    {
        std::string deviceName;
        if (aa_proto::parseServiceDiscoveryRequest(data, length, deviceName) && !deviceName.empty())
            _phoneName = deviceName;
        log_i("Service discovery request from '%s'", _phoneName.c_str());
        queueFrame(AA_CH_CONTROL, AA_MSG_SERVICE_DISCOVERY_RESPONSE,
                   aa_proto::serviceDiscoveryResponse());
        _state = PROTOCOL_STATUS_ONLINE;
        break;
    }

    case AA_MSG_PING_REQUEST:
    {
        int64_t timestamp = 0;
        aa_proto::parsePingRequest(data, length, timestamp);
        queueFrame(AA_CH_CONTROL, AA_MSG_PING_RESPONSE, aa_proto::pingResponse(timestamp),
                   AA_FLAG_PLAINTEXT);
        break;
    }

    case AA_MSG_PING_RESPONSE:
        _lastPongReceived = nowMs();
        break;

    case AA_MSG_AUDIO_FOCUS_REQUEST:
    {
        int32_t requestType = 0;
        aa_proto::parseAudioFocusRequest(data, length, requestType);
        log_d("Audio focus request %d", requestType);
        queueFrame(AA_CH_CONTROL, AA_MSG_AUDIO_FOCUS_NOTIFICATION,
                   aa_proto::audioFocusNotification(requestType));
        break;
    }

    case AA_MSG_NAV_FOCUS_REQUEST:
        queueFrame(AA_CH_CONTROL, AA_MSG_NAV_FOCUS_NOTIFICATION, aa_proto::navFocusNotification());
        break;

    case AA_MSG_BYEBYE_REQUEST:
        log_i("Phone requested shutdown");
        queueFrame(AA_CH_CONTROL, AA_MSG_BYEBYE_RESPONSE, aa_proto::byeByeResponse());
        _connected = false;
        break;

    case AA_MSG_BYEBYE_RESPONSE:
        _connected = false;
        break;

    case AA_MSG_VOICE_SESSION_NOTIFICATION:
        log_d("Voice session notification");
        break;

    default:
        log_d("Unhandled control message 0x%04x (%zu bytes)", msgId, length);
        break;
    }
}

void AaConnection::handleMedia(uint8_t channel, uint16_t msgId, const uint8_t *data, size_t length)
{
    ChannelState &state = _channels[channel];

    switch (msgId)
    {
    case AA_MSG_MEDIA_SETUP:
        log_i("Media setup on channel %d", channel);
        queueFrame(channel, AA_MSG_MEDIA_CONFIG,
                   aa_proto::mediaSetupResponse(Settings::aaMaxUnacked));
        if (channel == AA_CH_VIDEO)
            queueFrame(AA_CH_VIDEO, AA_MSG_VIDEO_FOCUS_NOTIFICATION,
                       aa_proto::videoFocusNotification(true, true));
        break;

    case AA_MSG_MEDIA_START:
        aa_proto::parseMediaStart(data, length, state.session);
        log_i("Media start on channel %d (session %d)", channel, state.session);
        if (channel == AA_CH_VIDEO)
            onPhoneConnect();
        break;

    case AA_MSG_MEDIA_STOP:
        log_i("Media stop on channel %d", channel);
        state.session = -1;
        break;

    case AA_MSG_VIDEO_FOCUS_REQUEST:
        queueFrame(AA_CH_VIDEO, AA_MSG_VIDEO_FOCUS_NOTIFICATION,
                   aa_proto::videoFocusNotification(true, false));
        break;

    case AA_MSG_MEDIA_DATA:
        if (length < 8)
            return;
        // u64BE timestamp, then the elementary stream / PCM payload.
        if (channel == AA_CH_VIDEO)
            emitVideo(data + 8, length - 8);
        else
            emitAudio(channel, data + 8, length - 8);
        queueFrame(channel, AA_MSG_MEDIA_ACK, aa_proto::mediaAck(state.session));
        break;

    case AA_MSG_MEDIA_CODEC_CONFIG:
        // H.264 SPS/PPS -- no timestamp, feed straight to the decoder.
        if (channel == AA_CH_VIDEO)
            emitVideo(data, length);
        queueFrame(channel, AA_MSG_MEDIA_ACK, aa_proto::mediaAck(state.session));
        break;

    default:
        log_d("Unhandled media message 0x%04x on channel %d", msgId, channel);
        break;
    }
}

void AaConnection::handleSensor(uint16_t msgId, const uint8_t *data, size_t length)
{
    if (msgId != AA_MSG_SENSOR_REQUEST)
    {
        log_d("Unhandled sensor message 0x%04x", msgId);
        return;
    }

    int32_t sensorType = 0;
    aa_proto::parseSensorRequest(data, length, sensorType);
    log_d("Sensor start request (type %d)", sensorType);
    queueFrame(AA_CH_SENSOR, AA_MSG_SENSOR_RESPONSE, aa_proto::sensorResponse());

    // Answer with an initial event right away: the phone won't project
    // until it has seen the driving status.
    if (sensorType == 13 /* SENSOR_DRIVING_STATUS_DATA */)
        queueFrame(AA_CH_SENSOR, AA_MSG_SENSOR_BATCH, aa_proto::sensorBatchDrivingStatus(0));
    else if (sensorType == 10 /* SENSOR_NIGHT_MODE */)
        queueFrame(AA_CH_SENSOR, AA_MSG_SENSOR_BATCH,
                   aa_proto::sensorBatchNightMode(Settings::nightMode == 1));
}

void AaConnection::handleInput(uint16_t msgId, const uint8_t *data, size_t length)
{
    (void)data;
    (void)length;
    if (msgId == AA_MSG_KEY_BINDING_REQUEST)
    {
        queueFrame(AA_CH_INPUT, AA_MSG_KEY_BINDING_RESPONSE, aa_proto::keyBindingResponse());
        return;
    }
    log_d("Unhandled input message 0x%04x", msgId);
}

void AaConnection::emitVideo(const uint8_t *data, size_t length)
{
    if (length == 0)
        return;
    std::unique_ptr<Message> message =
        Message::Payload(CMD_VIDEO_DATA, length, AV_INPUT_BUFFER_PADDING_SIZE);
    if (!message->data())
    {
        log_w("Video frame dropped > can't allocate %zu bytes", length);
        return;
    }
    memcpy(message->data(), data, length);
    videoStream.pushDiscard(std::move(message));
}

void AaConnection::emitAudio(uint8_t channel, const uint8_t *data, size_t length)
{
    if (length == 0)
        return;
    // Carlinkit audio shape: u32LE format type at 0 (4 = 48k stereo,
    // 5 = 16k mono), PCM from byte 12.
    bool media = channel == AA_CH_MEDIA_AUDIO;
    std::unique_ptr<Message> message = Message::Payload(CMD_AUDIO_DATA, length + AUDIO_BUFFER_OFFSET);
    uint8_t *buffer = message->data();
    if (!buffer)
    {
        log_w("Audio segment dropped > can't allocate %zu bytes", length);
        return;
    }
    writeU32le(buffer, media ? 4 : 5);
    writeU32le(buffer + 4, 0);
    writeU32le(buffer + 8, 0);
    memcpy(buffer + AUDIO_BUFFER_OFFSET, data, length);
    message->setOffset(AUDIO_BUFFER_OFFSET);
    (media ? audioStreamMain : audioStreamAux).pushDiscard(std::move(message));
}

void AaConnection::queueFrame(uint8_t channel, uint16_t msgId, const std::vector<uint8_t> &body,
                              uint8_t flagClass)
{
    std::unique_ptr<Message> message(new Message(CMD_AA_FRAME, false, AA_FRAME_HEAD + body.size()));
    uint8_t *data = message->data();
    if (!data)
        return;
    data[0] = channel;
    data[1] = flagClass;
    data[2] = msgId >> 8;
    data[3] = msgId & 0xff;
    if (!body.empty())
        memcpy(data + AA_FRAME_HEAD, body.data(), body.size());
    if (!writeQueue.pushDiscard(std::move(message)))
        log_w("Write queue full, AA frame 0x%04x dropped", msgId);
}

bool AaConnection::sendFrame(libusb_device_handle *handler, uint8_t ep, uint8_t channel,
                             uint16_t msgId, const uint8_t *body, size_t length, uint8_t flagClass)
{
    bool plaintext = flagClass == AA_FLAG_PLAINTEXT;
    bool control = flagClass == AA_FLAG_ENC_CONTROL;

    // Assemble msgId + body, encrypt everything except plaintext-class
    // messages (version/ssl/auth/ping), then fragment.
    std::vector<uint8_t> plain(2 + length);
    plain[0] = msgId >> 8;
    plain[1] = msgId & 0xff;
    if (length)
        memcpy(plain.data() + 2, body, length);

    std::vector<uint8_t> payload;
    if (plaintext)
    {
        payload = std::move(plain);
    }
    else
    {
        char error[256] = {0};
        if (!_ssl.encrypt(plain.data(), plain.size(), payload, error))
        {
            log_w("TLS encrypt failed > %s", error);
            return false;
        }
    }

    size_t offset = 0;
    std::vector<uint8_t> frame;
    while (offset < payload.size() || payload.empty())
    {
        size_t chunk = std::min<size_t>(AA_MAX_FRAME_PAYLOAD, payload.size() - offset);
        bool first = offset == 0;
        bool last = offset + chunk == payload.size();
        uint8_t flags = (first ? AA_FRAME_FIRST : 0) | (last ? AA_FRAME_LAST : 0) |
                        (control ? AA_FRAME_CONTROL : 0) | (plaintext ? 0 : AA_FRAME_ENCRYPTED);

        frame.clear();
        frame.push_back(channel);
        frame.push_back(flags);
        frame.push_back((chunk >> 8) & 0xff);
        frame.push_back(chunk & 0xff);
        if (first && !last)
        {
            uint32_t total = payload.size();
            frame.push_back((total >> 24) & 0xff);
            frame.push_back((total >> 16) & 0xff);
            frame.push_back((total >> 8) & 0xff);
            frame.push_back(total & 0xff);
        }
        frame.insert(frame.end(), payload.begin() + offset, payload.begin() + offset + chunk);

        int transferred = 0;
        log_d("TX ch %d id 0x%04x flags 0x%02x wire %zu", channel, msgId, flags, frame.size());
        int status = libusb_bulk_transfer(handler, ep, frame.data(), frame.size(), &transferred,
                                          AA_HEARTBEAT_DELAY);
        if (status != LIBUSB_SUCCESS || transferred != (int)frame.size())
        {
            log_w("Bulk write failed ch %d id 0x%04x wire %zu > %s", channel, msgId, frame.size(),
                  libusb_error_name(status));
            return false;
        }

        offset += chunk;
        if (payload.empty())
            break;
    }
    return true;
}

bool AaConnection::sendKey(libusb_device_handle *handler, uint8_t ep, uint32_t keycode)
{
    if (!_channels[AA_CH_INPUT].open)
        return false;
    aa_proto::Bytes down = aa_proto::inputReportKey(nowNs(), keycode, true);
    aa_proto::Bytes up = aa_proto::inputReportKey(nowNs(), keycode, false);
    return sendFrame(handler, ep, AA_CH_INPUT, AA_MSG_INPUT_REPORT, down.data(), down.size(),
                     AA_FLAG_ENC_SIGNAL) &&
           sendFrame(handler, ep, AA_CH_INPUT, AA_MSG_INPUT_REPORT, up.data(), up.size(),
                     AA_FLAG_ENC_SIGNAL);
}

// Translate the Carlinkit-shaped Messages the application layer produces.
bool AaConnection::translate(const Message &message, libusb_device_handle *handler, uint8_t ep)
{
    switch (message.type())
    {
    case CMD_AA_FRAME:
    {
        const uint8_t *data = message.data();
        int32_t length = message.length();
        if (!data || length < AA_FRAME_HEAD)
            return true;
        uint16_t msgId = (data[2] << 8) | data[3];
        return sendFrame(handler, ep, data[0], msgId, data + AA_FRAME_HEAD,
                         length - AA_FRAME_HEAD, data[1]);
    }

    case CMD_TOUCH:
    {
        if (!_auth || !_channels[AA_CH_INPUT].open)
            return true;
        // u32LE action (14 down / 15 move / 16 up), x, y as 10000*normalized.
        uint32_t action = message.getInt(0);
        int aaAction = action == 14 ? AA_TOUCH_DOWN : action == 16 ? AA_TOUCH_UP : AA_TOUCH_MOVED;
        int width, height;
        aaVideoSize(width, height);
        uint32_t x = (uint64_t)message.getInt(4) * width / 10000;
        uint32_t y = (uint64_t)message.getInt(8) * height / 10000;
        aa_proto::Bytes report = aa_proto::inputReportTouch(nowNs(), x, y, aaAction);
        return sendFrame(handler, ep, AA_CH_INPUT, AA_MSG_INPUT_REPORT, report.data(),
                         report.size(), AA_FLAG_ENC_SIGNAL);
    }

    case CMD_MULTI_TOUCH:
    {
        if (!_auth || !_channels[AA_CH_INPUT].open)
            return true;
        // Payload = 16 bytes/contact: x(float), y(float), action(u32), id(u32),
        // where action is MT_ACTION_DOWN/MOVE/UP (see protocol_const.h).
        const uint8_t *data = message.data();
        int32_t length = message.length();
        int count = data ? length / 16 : 0;
        if (count <= 0)
            return true;

        int width, height;
        aaVideoSize(width, height);

        aa_proto::TouchPoint points[MUTLITOUCH_MAX_TOUCH];
        int n = 0, changedIndex = -1;
        uint32_t changedAction = MT_ACTION_MOVE;
        for (int i = 0; i < count && n < MUTLITOUCH_MAX_TOUCH; i++)
        {
            const uint8_t *p = data + i * 16;
            float fx, fy;
            memcpy(&fx, p, sizeof(fx));
            memcpy(&fy, p + 4, sizeof(fy));
            uint32_t state = readU32le(p + 8);
            points[n].x = (uint32_t)(fx * width);
            points[n].y = (uint32_t)(fy * height);
            points[n].id = readU32le(p + 12);
            // Remember which contact changed this frame -> AA needs a single
            // action + action_index for the whole report.
            if (state == MT_ACTION_DOWN || state == MT_ACTION_UP)
            {
                changedIndex = n;
                changedAction = state;
            }
            n++;
        }

        int actionIndex = changedIndex >= 0 ? changedIndex : 0;
        int aaAction;
        if (changedAction == MT_ACTION_DOWN)
            aaAction = (n == 1) ? AA_TOUCH_DOWN : AA_TOUCH_POINTER_DOWN;
        else if (changedAction == MT_ACTION_UP)
            aaAction = (n == 1) ? AA_TOUCH_UP : AA_TOUCH_POINTER_UP;
        else
            aaAction = AA_TOUCH_MOVED;

        aa_proto::Bytes report =
            aa_proto::inputReportMultiTouch(nowNs(), points, n, aaAction, actionIndex);
        return sendFrame(handler, ep, AA_CH_INPUT, AA_MSG_INPUT_REPORT, report.data(),
                         report.size(), AA_FLAG_ENC_SIGNAL);
    }

    case CMD_CONTROL:
    {
        if (!_auth)
            return true;
        int button = message.getInt(0);
        switch (button)
        {
        case BTN_LEFT:
            return sendKey(handler, ep, AA_KEY_DPAD_LEFT);
        case BTN_RIGHT:
            return sendKey(handler, ep, AA_KEY_DPAD_RIGHT);
        case 113: // BTN_UP (not in protocol_const.h)
            return sendKey(handler, ep, AA_KEY_DPAD_UP);
        case BTN_DOWN:
            return sendKey(handler, ep, AA_KEY_DPAD_DOWN);
        case BTN_SELECT_DOWN:
            return sendKey(handler, ep, AA_KEY_DPAD_CENTER);
        case BTN_SELECT_UP:
            return true; // center key is sent as a full press by SELECT_DOWN
        case BTN_BACK:
            return sendKey(handler, ep, AA_KEY_BACK);
        case BTN_HOME:
            return sendKey(handler, ep, AA_KEY_HOME);
        case BTN_PLAY:
            return sendKey(handler, ep, AA_KEY_MEDIA_PLAY);
        case BTN_PAUSE:
            return sendKey(handler, ep, AA_KEY_MEDIA_PAUSE);
        case BTN_203:
            return sendKey(handler, ep, AA_KEY_PLAY_PAUSE);
        case BTN_NEXT_TRACK:
            return sendKey(handler, ep, AA_KEY_MEDIA_NEXT);
        case BTN_PREVIOUS_TRACK:
            return sendKey(handler, ep, AA_KEY_MEDIA_PREVIOUS);
        case BTN_SIRI:
            return sendKey(handler, ep, AA_KEY_SEARCH);
        case 16: // night mode on
        case 17: // night mode off
        {
            if (!_channels[AA_CH_SENSOR].open)
                return true;
            aa_proto::Bytes batch = aa_proto::sensorBatchNightMode(button == 16);
            return sendFrame(handler, ep, AA_CH_SENSOR, AA_MSG_SENSOR_BATCH, batch.data(),
                             batch.size(), AA_FLAG_ENC_SIGNAL);
        }
        case 500: // video focus
        case 501: // video release
        {
            if (!_channels[AA_CH_VIDEO].open)
                return true;
            aa_proto::Bytes focus = aa_proto::videoFocusNotification(button == 500, true);
            return sendFrame(handler, ep, AA_CH_VIDEO, AA_MSG_VIDEO_FOCUS_NOTIFICATION,
                             focus.data(), focus.size(), AA_FLAG_ENC_SIGNAL);
        }
        default:
            log_v("Button %d has no Android Auto mapping", button);
            return true;
        }
    }

    case CMD_HEARTBEAT:
    {
        if (!_auth)
            return true;
        _lastPingSent = nowMs();
        aa_proto::Bytes ping = aa_proto::pingRequest(nowNs());
        return sendFrame(handler, ep, AA_CH_CONTROL, AA_MSG_PING_REQUEST, ping.data(), ping.size(),
                         AA_FLAG_PLAINTEXT);
    }

    default:
        log_v("Message type %d has no Android Auto mapping", message.type());
        return true;
    }
}

void AaConnection::writeLoop(libusb_device_handle *handler, uint8_t ep)
{
    while (_connected)
    {
        std::unique_ptr<Message> message = writeQueue.pop();
        if (!message)
        {
            if (!writeQueue.waitFor(_connected, AA_HEARTBEAT_DELAY))
                break;
            message = writeQueue.pop();
        }

        // Idle: keep the link alive and watch for a silent phone.
        if (!message)
        {
            int64_t pong = _lastPongReceived.load();
            if (_auth && pong > 0 && nowMs() - pong > AA_PING_TIMEOUT)
            {
                log_w("Ping timeout, reconnecting");
                _connected = false;
                break;
            }
            message = Message::HeartBeat();
        }

        if (!_connected)
            break;

        if (!message->allocated())
            continue;

        // Collapse queued motion events like the Carlinkit backend does.
        while (message->isMotion() && writeQueue.peek() && writeQueue.peek()->isMotion())
        {
            message = writeQueue.pop();
        }

        if (!translate(*message, handler, ep))
        {
            log_w("Send failed, reconnecting");
            _connected = false;
        }
    }
}
