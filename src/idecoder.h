#ifndef SRC_IDECODER
#define SRC_IDECODER

extern "C"
{
#include <libavcodec/avcodec.h> // AVCodecID
}

#include "struct/video_buffer.h"
#include "struct/atomic_queue.h"
#include "protocol/message.h"
#include "settings.h"

// Common interface for the video-decoder backends so the backend can be chosen
// at runtime (the "cedar-decode" setting): the software avcodec Decoder, or the
// hardware CedarDecoder (compiled in only when built with USE_CEDAR). Both push
// frames into the shared VideoBuffer for the renderer.
class IDecoder
{
public:
    IDecoder() : buffer(Settings::renderingBuffer) {}
    virtual ~IDecoder() = default;

    virtual void start(AtomicQueue<Message> *data, AVCodecID codecId) = 0;
    virtual void stop() = 0;
    virtual void flush() = 0;

    VideoBuffer buffer;
};

#endif /* SRC_IDECODER */
