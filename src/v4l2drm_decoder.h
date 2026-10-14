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

    void start(AtomicQueue<Message> *data, AVCodecID codecId, bool resyncOnDrop) override;
    void stop() override;
    void flush() override;
    bool failed() const override { return _failed.load(); }

private:
    void runner();
    bool setup(AVCodecID codecId);
    void teardown();
    void loop(AVPacket *packet, AVFrame *frame);
    // Scanout lifetime: keep the most recently presented frames referenced
    // so the display engine never scans a buffer the decoder is writing into
    // (see holdShown()).
    void holdShown(AVFrame *&frame);
    void releaseHeld();
    // Every frame is decoded (each one is a reference), but only the newest
    // is presented: a frame the display can't take yet waits in _pending,
    // where a newer frame supersedes it, and is retried after the flip.
    void present(AVFrame *frame);
    void retryPending();
    void dropPending();
    void logStats();

    std::thread _thread;
    std::atomic<bool> _active;
    std::atomic<bool> _failed{false}; // set when this backend gives up (-> software)
    AtomicQueue<Message> *_data;
    AVCodecID _codecId;
    bool _resyncOnDrop = false;
    uint32_t _dropsAtStart = 0;
    AVFrame *_pending = nullptr;
    bool _hasPending = false;
    struct
    {
        uint32_t input = 0, decoded = 0, shown = 0, superseded = 0, dropped = 0;
        uint64_t decodeUs = 0;
        uint32_t decodeUsMax = 0;
        uint16_t depthMax = 0;
        uint32_t since = 0;
    } _stats;

    AVCodecContext *_ctx;
    // The ffmpeg V4L2-request (stateless cedrus) decoder needs libavcodec's
    // H.264 parser to populate the per-slice parameters it decodes from --
    // feeding it raw Annex-B access units directly fails with "Invalid data".
    AVCodecParserContext *_parser;
    AVBufferRef *_hwdev;
    static constexpr int HELD_FRAMES = 3;
    AVFrame *_held[HELD_FRAMES] = {};
    int _heldNext = 0;
};

#endif /* SRC_V4L2DRM_DECODER */
