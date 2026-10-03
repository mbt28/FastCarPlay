#ifndef SRC_V4L2DRM_DECODER
#define SRC_V4L2DRM_DECODER

// Mainline-cedrus H.264 decoder (blob-free).  Decodes through ffmpeg's V4L2
// Request API hwaccel, which drives the in-kernel sunxi-cedrus stateless decoder
// (/dev/media0). Each frame comes back as AV_PIX_FMT_DRM_PRIME -- a tiled-NV12
// dma-buf -- which we present on the sun4i DEFE front-end via the atomic API
// (DRM_FORMAT_NV12 + DRM_FORMAT_MOD_ALLWINNER_TILED): HW de-tile + CSC + scale,
// zero-copy, no vendor blobs. Implements the IDecoder surface so
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
    bool failed() const override { return _failed.load(); }

private:
    void runner();
    bool setup(AVCodecID codecId);
    void teardown();
    void loop(AVPacket *packet, AVFrame *frame);
    // Scanout lifetime: keep the two most recently presented frames
    // referenced so the display engine never scans a buffer the decoder is
    // writing into (see loop()).
    void holdShown(AVFrame *&frame);
    void releaseHeld();

    std::thread _thread;
    std::atomic<bool> _active;
    std::atomic<bool> _failed{false}; // set when this backend gives up (-> software)
    AtomicQueue<Message> *_data;
    AVCodecID _codecId;

    AVCodecContext *_ctx;
    // The ffmpeg V4L2-request (stateless cedrus) decoder needs libavcodec's
    // H.264 parser to populate the per-slice parameters it decodes from --
    // feeding it raw Annex-B access units directly fails with "Invalid data".
    AVCodecParserContext *_parser;
    AVBufferRef *_hwdev;
    AVFrame *_held[2] = {nullptr, nullptr};
    int _heldNext = 0;
};

#endif /* SRC_V4L2DRM_DECODER */
