#include "decoder.h"

#include <iostream>
#include "common/logger.h"
#include "common/functions.h"
#include "settings.h"

Decoder::Decoder()
    : _context(nullptr),
      _active(false),
      _data(nullptr)
{
}

Decoder::~Decoder()
{
    stop();
}

void Decoder::start(AtomicQueue<Message> *data, AVCodecID codecId)
{
    if (_active)
        stop();

    buffer.reset();
    _data = data;
    _codecId = codecId;
    _active = true;
    _thread = std::thread(&Decoder::runner, this);
}

void Decoder::stop()
{
    if (!_active)
        return;
    _active = false;
    _data->notify();
    if (_thread.joinable())
        _thread.join();
}

void Decoder::flush()
{
    if (_context)
        avcodec_flush_buffers(_context);
}

// Open the software decoder for this codec.
//
// This backend is deliberately software-only. Hardware decode is decided once,
// centrally, by video_path (which asks the kernel what the chip can actually
// decode) and served by V4l2DrmDecoder, which does the part that makes it worth
// having: a hwdevice context plus a get_format that keeps frames in driver
// buffers (DRM_PRIME) all the way to a DRM plane.
//
// This used to iterate every registered codec and open the first one flagged
// AV_CODEC_CAP_HARDWARE. That selected by capability flag rather than by what
// the board contains -- on a Pi it would try hevc_cuvid (registered because
// ffmpeg is built with NVIDIA support) on a machine with no NVIDIA GPU, and on
// a board exposing a V4L2 M2M node it could succeed and then quietly decode
// through system memory, bypassing the video_path decision entirely. Since it
// set up no hwdevice or get_format, it never actually accelerated anything.
AVCodecContext *Decoder::load_codec(AVCodecID codec_id)
{
    const AVCodec *codec = nullptr;
    AVCodecContext *result = nullptr;

    codec = avcodec_find_decoder(codec_id);
    if (!codec)
    {
        log_w("[Video] no decoder for codec id %d", codec_id);
        return nullptr;
    }

    result = avcodec_alloc_context3(codec);
    if (!result)
    {
        log_w("Failed to allocate context for codec id %d", codec_id);
        return nullptr;
    }

    // Projection is latency-sensitive, so these matter here: they used to be set
    // only on the hardware attempt above, which meant the software decoder --
    // the path actually taken almost everywhere -- ignored both settings.
    if (Settings::codecLowDelay)
        result->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (Settings::codecFast)
        result->flags2 |= AV_CODEC_FLAG2_FAST;

    int ret = avcodec_open2(result, codec, nullptr);
    if (ret < 0)
    {
        log_w("Failed to open SW decoder %s > %s", codec->name, avErrorText(ret).c_str());
        avcodec_free_context(&result);
        return nullptr;
    }

    log_i("SW decoder %s", codec->name);
    return result;
}

void Decoder::runner()
{
    // Set thread name
    setThreadName("video-decoder");

    // Load codec context
    _context = load_codec(_codecId);
    if (!_context)
    {
        log_e("Can't find decoder for codec %s", avcodec_get_name(_codecId));
        return;
    }
    std::string codec = _context->codec->name;

    // Initialize parser for the codec
    AVCodecParserContext *parser = av_parser_init(_codecId);
    if (!parser)
        log_e("Can't initilise parser for codec %s", codec.c_str());
    else
    {
        // Allocate packet for decoding
        AVPacket *packet = av_packet_alloc();
        if (!packet)
            log_e("Can't allocate packet for codec %s", codec.c_str());
        else
        {
            // Allocate frame for decoded data
            AVFrame *frame = av_frame_alloc();
            if (!frame)
                log_e("Can't allocate frame for codec %s", codec.c_str());
            else
            {
                loop(_context, parser, packet, frame); // Run decoding loop
                av_frame_free(&frame);
            }
            av_packet_free(&packet);
        }
        av_parser_close(parser);
    }
    avcodec_free_context(&_context);
    _context = nullptr;
}

void Decoder::loop(AVCodecContext *context, AVCodecParserContext *parser, AVPacket *packet, AVFrame *frame)
{
    uint32_t counter = 0;

    // Main decoding loop; runs until global_quit flag is set
    while (_data->wait(_active))
    {
        // Get raw data segment from queue
        std::unique_ptr<Message> segment = _data->pop();
        uint8_t *data_ptr = segment->data();
        int data_size = segment->length();

        // Feed raw data into the parser and decoder
        while (_active && data_size > 0)
        {
            uint8_t *paket_data;
            int paket_size;

            // Parse raw data into packets
            int len = av_parser_parse2(parser, context,
                                       &paket_data, &paket_size,
                                       data_ptr, data_size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);

            // Parsing error; break out
            if (len < 0)
                break;

            // Move forward through segment
            data_ptr += len;
            data_size -= len;

            if (paket_size <= 0)
                continue;

            // Load packet data
            av_packet_unref(packet);
            packet->data = paket_data;
            packet->size = paket_size;

            // Send packet to decoder
            int send_ret = avcodec_send_packet(context, packet);
            if (send_ret != 0)
            {
                log_w("Can't decode packet > %s", avErrorText(send_ret).c_str());
                continue;
            }
            // Receive decoded frames
            while (avcodec_receive_frame(context, frame) == 0 && _active)
            {
                AVFrame *out = buffer.write(counter++);
                if (out)
                {
                    av_frame_unref(out);
                    av_frame_move_ref(out, frame);
                    buffer.commit();
                }
            }
        }
    }

    // push null packet to flush decoder and drain delayed frames
    if (_context)
    {
        avcodec_send_packet(context, nullptr);
        while (avcodec_receive_frame(context, frame) == 0)
        {
            AVFrame *out = buffer.write(counter++);
            if (out)
            {
                av_frame_unref(out);
                av_frame_move_ref(out, frame);
                buffer.commit();
            }
        }
    }
}
