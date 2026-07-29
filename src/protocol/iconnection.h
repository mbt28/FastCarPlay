#ifndef SRC_PROTOCOL_ICONNECTION
#define SRC_PROTOCOL_ICONNECTION

extern "C"
{
#include <libavcodec/avcodec.h> // AVCodecID
}

#include <atomic>
#include <memory>
#include <string>

#include "struct/atomic_queue.h"
#include "protocol/message.h"
#include "protocol/protocol_const.h"

#define WRITE_QUEUE_SIZE 128
#define VIDEO_QUEUE_SIZE 128
#define AUDIO_QUEUE_SIZE 128

// Protocol backend interface. Consumers (application loops, decoders, audio,
// input) interact only through the queues and the members below, so any
// backend that fills videoStream/audioStream* with Carlinkit-shaped Messages
// and consumes writeQueue works with the rest of the application unchanged.
class IConnection
{
public:
    IConnection()
        : writeQueue(WRITE_QUEUE_SIZE),
          videoStream(VIDEO_QUEUE_SIZE),
          audioStreamMain(AUDIO_QUEUE_SIZE),
          audioStreamAux(AUDIO_QUEUE_SIZE),
          _state(PROTOCOL_STATUS_INITIALISING),
          _method("unknown"),
          _phoneName("phone"),
          _transfered(0)
    {
    }

    IConnection(const IConnection &) = delete;
    IConnection &operator=(const IConnection &) = delete;

    virtual ~IConnection() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual const std::string status() const = 0;

    // The codec of the frames fed into videoStream. Carlinkit and Android Auto
    // are H.264; wireless CarPlay negotiates HEVC. The decoder is started with
    // this, so a backend that streams H.265 must override it.
    virtual AVCodecID videoCodec() const { return AV_CODEC_ID_H264; }

    // True while the phone is actually projecting. A session can be connected
    // but backgrounded -- the user pressed exit, the phone handed the screen
    // back and is waiting rather than disconnecting. The app then shows its
    // own UI without tearing anything down.
    virtual bool videoFocused() const { return true; }
    // Ask the phone to project again. No-op for backends that never release.
    virtual void requestVideoFocus() {}

    bool inline send(std::unique_ptr<Message> message) { return writeQueue.pushDiscard(std::move(message)); }
    uint32_t transfered() const { return _transfered.load(std::memory_order_acquire); }

    int8_t state() const { return _state.load(); }
    std::string connectionMethod() const { return _method; }
    std::string phoneName() const { return _phoneName; }

    AtomicQueue<Message> writeQueue;
    AtomicQueue<Message> videoStream;
    AtomicQueue<Message> audioStreamMain;
    AtomicQueue<Message> audioStreamAux;

protected:
    std::atomic<int8_t> _state;
    std::string _method;
    std::string _phoneName;
    std::atomic<uint32_t> _transfered;
};

#endif /* SRC_PROTOCOL_ICONNECTION */
