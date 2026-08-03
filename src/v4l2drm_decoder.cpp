#include "v4l2drm_decoder.h"

#ifdef USE_CEDRUS

#include <cstring>
#include <cstdio>
#include <cerrno>

#include "common/logger.h"
#include "common/functions.h"
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
// (drm_display owns the session; the UI overlay shares it).
void drm_show(AVFrame *frame)
{
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor *)frame->data[0];
    if (!d || d->nb_objects < 1 || d->nb_layers < 1) return;
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

    drm_display::showVideo(layer->format, frame->width, frame->height,
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

void V4l2DrmDecoder::start(AtomicQueue<Message> *data, AVCodecID codecId)
{
    if (_active)
        stop();
    buffer.reset();
    _data = data;
    _codecId = codecId;
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

    log_i("[V4L2-DRM] ffmpeg v4l2-request hwaccel ready (%s -> cedrus -> DEFE)", codec->name);
    return true;
}

void V4l2DrmDecoder::teardown()
{
    if (_parser) { av_parser_close(_parser); _parser = nullptr; }
    if (_ctx) avcodec_free_context(&_ctx);
    if (_hwdev) av_buffer_unref(&_hwdev);
    drm_display::close();
}

void V4l2DrmDecoder::runner()
{
    setThreadName("cedrus-decoder");

    if (!setup(_codecId))
    {
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
    while (_data->wait(_active))
    {
        std::unique_ptr<Message> segment = _data->pop();
        uint8_t *data_ptr = segment->data();
        int data_size = segment->length();

        while (_active && data_size > 0)
        {
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

            int send_ret = avcodec_send_packet(_ctx, packet);
            if (send_ret != 0)
            {
                log_w("[V4L2-DRM] can't decode packet > %s", avErrorText(send_ret).c_str());
                continue;
            }
            while (avcodec_receive_frame(_ctx, frame) == 0 && _active)
            {
                if (frame->format == AV_PIX_FMT_DRM_PRIME)
                    drm_show(frame);
                av_frame_unref(frame);
            }
        }
    }
}

#endif /* USE_CEDRUS */
