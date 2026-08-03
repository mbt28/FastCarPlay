#ifndef SRC_V4L2DRM_DECODER
#define SRC_V4L2DRM_DECODER

// Mainline-cedrus H.264 decoder (blob-free).  Decodes through ffmpeg's V4L2
// Request API hwaccel, which drives the in-kernel sunxi-cedrus stateless decoder
// (/dev/media0). Each frame comes back as AV_PIX_FMT_DRM_PRIME -- a tiled-NV12
// dma-buf -- which we present on the sun4i DEFE front-end via the atomic API
// (DRM_FORMAT_NV12 + DRM_FORMAT_MOD_ALLWINNER_TILED): HW de-tile + CSC + scale,
// zero-copy, no libcedarc/ION. Same public surface as CedarDecoder so
// application.cpp can pick it behind USE_CEDRUS (decoder = cedrus).

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <thread>

#include "idecoder.h"
#include "struct/atomic_queue.h"
#include "protocol/message.h"

class V4l2DrmDecoder : public IDecoder
{
public:
    V4l2DrmDecoder();
    ~V4l2DrmDecoder();

    void start(AtomicQueue<Message> *data, AVCodecID codecId) override;
    void stop() override;
    void flush() override;

private:
    void runner();
    bool setup(AVCodecID codecId);
    void teardown();
    void loop(AVPacket *packet, AVFrame *frame);

    std::thread _thread;
    std::atomic<bool> _active;
    AtomicQueue<Message> *_data;
    AVCodecID _codecId;

    AVCodecContext *_ctx;
    AVCodecParserContext *_parser;
    AVBufferRef *_hwdev;
};

#endif /* SRC_V4L2DRM_DECODER */
