#include "v4l2drm_decoder.h"

#ifdef USE_CEDRUS

#include <cstring>
#include <cstdio>
#include <cerrno>

#include "common/logger.h"
#include "common/functions.h"
#include "common/annexb.h"
#include "common/clock.h"
#include "settings.h"

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
}

#include <drm_fourcc.h>

#include "drm_display.h"

// ---------------------------------------------------------------------------
// DEFE atomic display.  cedrus' CAPTURE buffer arrives as a drm_prime dma-buf
// (tiled NV12, ALLWINNER_TILED); we import it as a DRM_FORMAT_NV12 framebuffer
// and commit it via the atomic API so sun4i routes the plane through the DEFE
// front-end (HW de-tile + BT.601 CSC + scale). Requires the suniv DEFE kernel
// fix (EN bit31 cleared + FIR filter bypassed). Mirrors cedar_decoder.cpp but
// takes its planes from ffmpeg's AVDRMFrameDescriptor instead of two ION fds.
// ---------------------------------------------------------------------------
namespace
{
// Present one decoded drm_prime frame on the shared DRM video plane
// (drm_display owns the session; the UI overlay shares it). On Shown the
// frame is (queued for) scanout -- the caller must then keep its buffer alive
// until a later frame has replaced it on the plane.
drm_display::Show drm_show(AVFrame *frame)
{
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor *)frame->data[0];
    if (!d || d->nb_objects < 1 || d->nb_layers < 1) return drm_display::Show::Failed;
    const AVDRMLayerDescriptor *layer = &d->layers[0];

    int fds[4] = {0};
    uint32_t pitches[4] = {0}, offsets[4] = {0};
    int np = layer->nb_planes < 4 ? layer->nb_planes : 4;
    for (int i = 0; i < np; i++)
    {
        int oi = layer->planes[i].object_index;
        fds[i] = d->objects[oi].fd;
        pitches[i] = layer->planes[i].pitch;
        offsets[i] = layer->planes[i].offset;
    }
    uint64_t modifier = d->objects[layer->planes[0].object_index].format_modifier;

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        fprintf(stderr, "[V4L2-DRM] frame %dx%d fmt=%.4s objs=%d planes=%d "
                "pitch0=%u pitch1=%u off1=%u mod=0x%llx\n",
                frame->width, frame->height, (const char *)&layer->format,
                d->nb_objects, layer->nb_planes, pitches[0], pitches[1], offsets[1],
                (unsigned long long)modifier);
    }

    return drm_display::showVideo(layer->format, frame->width, frame->height,
                                  frame->width, frame->height,
                                  np, fds, pitches, offsets, modifier, "Cedrus");
}

// get_format: select the V4L2-Request (DRM_PRIME) hwaccel when offered.
enum AVPixelFormat get_drm_format(AVCodecContext *, const enum AVPixelFormat *fmts)
{
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    return fmts[0];
}

} // namespace

V4l2DrmDecoder::V4l2DrmDecoder()
    : _active(false), _data(nullptr), _ctx(nullptr), _parser(nullptr), _hwdev(nullptr)
{
}

V4l2DrmDecoder::~V4l2DrmDecoder()
{
    stop();
}

void V4l2DrmDecoder::start(AtomicQueue<Message> *data, AVCodecID codecId, bool resyncOnDrop)
{
    if (_active)
        stop();
    buffer.reset();
    _data = data;
    _codecId = codecId;
    _resyncOnDrop = resyncOnDrop;
    // Counted from here, so frames dropped while setup() runs (the queue
    // fills before the first pop) also trigger a resync.
    _dropsAtStart = data ? data->drops() : 0;
    _active = true;
    _thread = std::thread(&V4l2DrmDecoder::runner, this);
}

void V4l2DrmDecoder::stop()
{
    if (!_active)
        return;
    _active = false;
    if (_data)
        _data->notify();
    if (_thread.joinable())
        _thread.join();
}

void V4l2DrmDecoder::flush()
{
    if (_ctx)
        avcodec_flush_buffers(_ctx);
}

bool V4l2DrmDecoder::setup(AVCodecID codecId)
{
    if (!drm_display::open("V4L2-DRM"))
        return false;

    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec)
    {
        log_e("[V4L2-DRM] no decoder for codec %s", avcodec_get_name(codecId));
        return false;
    }

    _ctx = avcodec_alloc_context3(codec);
    if (!_ctx)
        return false;

    // V4L2 Request hwaccel is a DRM-typed hwdevice; get_format selects DRM_PRIME
    // so the in-kernel cedrus decoder is used and frames come back as dma-bufs.
    if (av_hwdevice_ctx_create(&_hwdev, AV_HWDEVICE_TYPE_DRM, nullptr, nullptr, 0) < 0)
    {
        log_e("[V4L2-DRM] can't create DRM hwdevice (v4l2-request)");
        return false;
    }
    _ctx->hw_device_ctx = av_buffer_ref(_hwdev);
    _ctx->get_format = get_drm_format;
    if (Settings::codecLowDelay)
        _ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (Settings::codecFast)
        _ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    int ret = avcodec_open2(_ctx, codec, nullptr);
    if (ret < 0)
    {
        log_e("[V4L2-DRM] avcodec_open2 failed: %s", avErrorText(ret).c_str());
        return false;
    }

    _parser = av_parser_init(codecId);
    if (!_parser)
    {
        log_e("[V4L2-DRM] can't init parser for %s", avcodec_get_name(codecId));
        return false;
    }
    // Every segment is exactly one access unit (CarPlay emits one decrypted
    // frame per segment, Android Auto one reassembled media message, and the
    // parameter-set config arrives as its own segment). Without this flag
    // the parser can only end a frame once it sees the START of the next one,
    // so every frame waited a full frame interval, and the last frame of any
    // burst -- the final state of a tap or a scroll -- stayed stuck until the
    // phone happened to send another. It also skips a copy and a byte scan
    // of every frame.
    _parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    log_i("[V4L2-DRM] ffmpeg v4l2-request hwaccel ready (%s -> cedrus -> DEFE)", codec->name);
    return true;
}

void V4l2DrmDecoder::teardown()
{
    // Give the held scanout buffers back before the context dies with them.
    releaseHeld();
    if (_pending)
        av_frame_free(&_pending);
    _hasPending = false;
    if (_parser) { av_parser_close(_parser); _parser = nullptr; }
    if (_ctx) avcodec_free_context(&_ctx);
    if (_hwdev) av_buffer_unref(&_hwdev);
    // The UI keeps the DRM session open across a decoder rebuild, so the
    // cached framebuffers (and their GEM handles pinning this decoder's
    // frame pool in CMA) must be dropped explicitly before close().
    drm_display::flushVideo();
    drm_display::close();
}

// Keep the last THREE presented frames referenced. Non-blocking flips leave
// commit N scanning while N+1 is queued, and sun4i signals a commit's flip
// one frame early when it starts before the previous commit has latched:
// sun4i_crtc_atomic_begin() publishes the new event before it waits for the
// previous LOADCTL, so the vblank that latches commit N also completes N+1.
// During continuous motion that early state persists, and the frame two
// commits back can still be on screen -- a buffer returned to the decoder
// then gets written mid-scan (a torn frame). Three deep covers it.
void V4l2DrmDecoder::holdShown(AVFrame *&frame)
{
    AVFrame *old = _held[_heldNext];
    if (old)
        av_frame_unref(old);
    else
        _held[_heldNext] = av_frame_alloc();
    if (_held[_heldNext])
        av_frame_move_ref(_held[_heldNext], frame);
    else
        av_frame_unref(frame); // allocation failed: tear risk over leak
    _heldNext = (_heldNext + 1) % HELD_FRAMES;
}

void V4l2DrmDecoder::releaseHeld()
{
    for (AVFrame *&f : _held)
    {
        if (f)
            av_frame_free(&f);
        f = nullptr;
    }
    _heldNext = 0;
}

void V4l2DrmDecoder::present(AVFrame *frame)
{
    switch (drm_show(frame))
    {
    case drm_display::Show::Shown:
        holdShown(frame); // on scanout: keep it alive, release an older one
        dropPending();    // older than what is now on screen
        _stats.shown++;
        break;
    case drm_display::Show::Busy:
        // The display is still flipping to an earlier frame. Keep this one
        // as the newest candidate; retryPending() shows it after the flip.
        if (!_pending)
            _pending = av_frame_alloc();
        if (!_pending)
            break; // allocation failed: lose this frame, the next one shows
        dropPending();
        av_frame_move_ref(_pending, frame);
        _hasPending = true;
        break;
    case drm_display::Show::Failed:
        break;
    }
}

void V4l2DrmDecoder::retryPending()
{
    switch (drm_show(_pending))
    {
    case drm_display::Show::Shown:
        holdShown(_pending); // moves the reference into the hold ring
        _hasPending = false;
        _stats.shown++;
        break;
    case drm_display::Show::Busy:
        break;
    case drm_display::Show::Failed:
        av_frame_unref(_pending);
        _hasPending = false;
        break;
    }
}

void V4l2DrmDecoder::dropPending()
{
    if (!_hasPending)
        return;
    av_frame_unref(_pending); // never reached the screen: safe to recycle
    _hasPending = false;
    _stats.superseded++;
}

void V4l2DrmDecoder::logStats()
{
    const uint32_t now = mono_ms();
    if (_stats.since == 0)
        _stats.since = now;
    const uint32_t span = now - _stats.since;
    if (span < 5000)
        return;
    if (_stats.input > 0 || _stats.dropped > 0)
        fprintf(stderr,
                "[Cedrus] in %u fps, decode %u fps (avg %u ms, max %u ms), shown %u fps, "
                "%u superseded, %u dropped, queue peak %u (%.1fs)\n",
                (unsigned)(_stats.input * 1000 / span), (unsigned)(_stats.decoded * 1000 / span),
                _stats.decoded ? (unsigned)(_stats.decodeUs / _stats.decoded / 1000) : 0u,
                (unsigned)(_stats.decodeUsMax / 1000), (unsigned)(_stats.shown * 1000 / span),
                _stats.superseded, _stats.dropped, (unsigned)_stats.depthMax, span / 1000.0);
    _stats = {};
    _stats.since = now;
}

void V4l2DrmDecoder::runner()
{
    setThreadName("cedrus-decoder");

    if (!setup(_codecId))
    {
        // Nothing will ever render on this backend -- say so, so the caller can
        // rebuild on software instead of showing a black screen.
        log_w("[V4L2-DRM] hardware decode unavailable for %s, falling back to software",
              avcodec_get_name(_codecId));
        _failed.store(true);
        teardown();
        return;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (packet && frame)
        loop(packet, frame);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);

    teardown();
}

void V4l2DrmDecoder::loop(AVPacket *packet, AVFrame *frame)
{
    // ~1s of frames: long enough to ride out a corrupt packet or a start before
    // the first keyframe, short enough that a real mismatch is caught quickly.
    constexpr int MAX_CONSECUTIVE_FAILURES = 30;
    // A decoded frame waiting for the display is retried this often even when
    // no new data arrives, so the last frame of a burst still reaches the
    // screen. A pending flip completes within one vblank (16.7 ms).
    constexpr uint32_t PENDING_RETRY_MS = 4;
    // After a drop, how long to hold the last good picture for the keyframe
    // the backend asked for before decoding through the gap anyway. Short: a
    // phone that ignores the request must cost a stutter, not a freeze.
    constexpr uint32_t RESYNC_TIMEOUT_MS = 300;
    int failures = 0;
    uint32_t seenDrops = _dropsAtStart;
    bool resync = false;
    uint32_t resyncSince = 0;

    for (;;)
    {
        const bool run = _hasPending ? _data->waitFor(_active, PENDING_RETRY_MS)
                                     : _data->wait(_active);
        if (!run)
            break;
        if (_hasPending)
            retryPending();
        logStats();

        const uint16_t depth = _data->count();
        std::unique_ptr<Message> segment = _data->pop();
        if (!segment)
            continue;
        _stats.input++;
        if (depth > _stats.depthMax)
            _stats.depthMax = depth;

        // A dropped frame breaks the reference chain: every frame after the
        // gap predicts from a picture the decoder never had, smearing garbage
        // over moving content until a keyframe. When the backend has asked
        // for one, skip to it and hold the last good picture meanwhile.
        const uint32_t drops = _data->drops();
        if (drops != seenDrops)
        {
            _stats.dropped += drops - seenDrops;
            seenDrops = drops;
            if (_resyncOnDrop && !resync)
            {
                log_w("[V4L2-DRM] video fell behind (frames dropped) -- holding the picture until a keyframe");
                resync = true;
                resyncSince = mono_ms();
            }
        }
        if (resync)
        {
            const uint32_t waited = mono_ms() - resyncSince;
            const AnnexBKinds kinds = annexbKinds(segment->data(), segment->length(),
                                                  _codecId == AV_CODEC_ID_HEVC);
            const bool key = kinds.keyframe;
            // Parameter sets always go through: CarPlay sends them as their
            // own segment, and the keyframe after them needs the new ones.
            if (!key && !kinds.paramSets && waited < RESYNC_TIMEOUT_MS)
                continue;
            // No avcodec_flush_buffers() here: on the v4l2-request hwaccel it
            // forces a full re-init that allocates a second cedrus buffer pool
            // next to the live one, which CMA cannot fit (ENOMEM -> ffmpeg
            // silently falls back to software decode). An IDR resets the
            // references by itself.
            if (key)
            {
                log_i("[V4L2-DRM] resynced at a keyframe after %u ms", (unsigned)waited);
                resync = false;
            }
            else if (!kinds.paramSets)
            {
                log_w("[V4L2-DRM] no keyframe within %u ms -- decoding through the gap",
                      (unsigned)waited);
                resync = false;
            }
            // (a parameter-set segment is decoded and the resync goes on)
        }

        uint8_t *data_ptr = segment->data();
        int data_size = segment->length();

        while (_active && data_size > 0)
        {
            // The stateless cedrus decoder needs the parser to extract the
            // slice parameters; feeding raw Annex-B straight in fails.
            uint8_t *pk_data;
            int pk_size;
            int len = av_parser_parse2(_parser, _ctx, &pk_data, &pk_size,
                                       data_ptr, data_size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (len < 0)
                break;
            data_ptr += len;
            data_size -= len;
            if (pk_size <= 0)
                continue;

            av_packet_unref(packet);
            packet->data = pk_data;
            packet->size = pk_size;

            const uint64_t t0 = mono_us();
            int send_ret = avcodec_send_packet(_ctx, packet);
            if (send_ret != 0)
            {
                // A packet with only parameter sets (CarPlay sends SPS/PPS as
                // a segment of their own) carries no picture: ffmpeg stores
                // the sets, then reports "no frame!" as INVALIDDATA.
                if (send_ret == AVERROR_INVALIDDATA &&
                    !annexbHasPicture(pk_data, pk_size, _codecId == AV_CODEC_ID_HEVC))
                    continue;
                log_w("[V4L2-DRM] can't decode packet > %s", avErrorText(send_ret).c_str());
                // A stream the hardware cannot actually handle fails on every
                // packet: the kernel can accept the format and still have no
                // block for this profile. Give up rather than stay blank -- but
                // only on a solid run of failures, so a corrupt packet or a
                // pre-keyframe start does not cost us the hardware path.
                if (++failures >= MAX_CONSECUTIVE_FAILURES)
                {
                    log_e("[V4L2-DRM] %d consecutive decode failures, falling back to software",
                          failures);
                    _failed.store(true);
                    return;
                }
                continue;
            }
            failures = 0;
            while (avcodec_receive_frame(_ctx, frame) == 0 && _active)
            {
                const uint32_t us = (uint32_t)(mono_us() - t0);
                _stats.decodeUs += us;
                if (us > _stats.decodeUsMax)
                    _stats.decodeUsMax = us;
                _stats.decoded++;
                if (frame->format == AV_PIX_FMT_DRM_PRIME)
                    present(frame);
                av_frame_unref(frame); // no-op when present() moved the ref out
            }
        }
    }
}

#endif /* USE_CEDRUS */
