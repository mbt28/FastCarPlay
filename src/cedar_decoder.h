#ifndef SRC_CEDAR_DECODER
#define SRC_CEDAR_DECODER

// Hardware H.264 decoder for Allwinner F1C200s (sunxi Cedar VE via libcedarc).
// Drop-in replacement for Decoder: same public surface (start/stop/flush +
// public VideoBuffer buffer) so application.cpp can pick it behind USE_CEDAR.
//
// libcedarc only emits PIXEL_FORMAT_YUV_MB32_420 (32x32 tiled NV12) on this SoC;
// we de-tile each picture into a linear NV12 AVFrame and push it through the
// same VideoBuffer, so the existing SDL2 renderer (which supports NV12) is
// untouched. libcedarc types are hidden behind void* to keep its C headers out
// of the rest of the C++ build.

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <thread>

#include "idecoder.h"
#include "struct/atomic_queue.h"
#include "protocol/message.h"

class CedarDecoder : public IDecoder
{
public:
    CedarDecoder();
    ~CedarDecoder();

    void start(AtomicQueue<Message> *data, AVCodecID codecId) override;
    void stop() override;
    void flush() override;

private:
    void runner();
    bool setup();
    void teardown();
    void feed(const uint8_t *data, int len);
    void drain(uint32_t &counter);

    std::thread _thread;
    std::atomic<bool> _active;
    AtomicQueue<Message> *_data;

    void *_dec;      // VideoDecoder*
    void *_memops;   // struct ScMemOpsS*
};

#endif /* SRC_CEDAR_DECODER */
