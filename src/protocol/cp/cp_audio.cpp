#include "cp_audio.h"

#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include "common/logger.h"

namespace cp_audio
{
namespace
{
// MPEG-4 sampling frequency index for the AAC AudioSpecificConfig.
int aacFreqIndex(int rate)
{
    switch (rate)
    {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    default: return 4; // 44.1k
    }
}

int16_t f2s16(float f)
{
    int v = (int)(f * 32767.0f + (f >= 0 ? 0.5f : -0.5f));
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}
} // namespace

Decoder::~Decoder()
{
    if (_frame) av_frame_free(&_frame);
    if (_pkt) av_packet_free(&_pkt);
    if (_ctx) avcodec_free_context(&_ctx);
}

bool Decoder::init(Codec codec, int rate, int channels, bool upmixToStereo)
{
    _codec = codec;
    _srcChannels = channels;
    _upmix = upmixToStereo && channels == 1;
    _outChannels = _upmix ? 2 : channels;
    _outRate = rate;

    if (codec == Codec::Pcm)
        return true; // no libavcodec context; passthrough + byte-swap

    const AVCodecID id = codec == Codec::AacLc ? AV_CODEC_ID_AAC : AV_CODEC_ID_OPUS;
    const AVCodec *dec = avcodec_find_decoder(id);
    if (!dec)
    {
        log_e("[cp-audio] no %s decoder in libavcodec", codec == Codec::AacLc ? "AAC" : "OPUS");
        return false;
    }
    _ctx = avcodec_alloc_context3(dec);
    if (!_ctx)
        return false;
    _ctx->sample_rate = rate;
    av_channel_layout_default(&_ctx->ch_layout, channels);

    if (codec == Codec::AacLc)
    {
        // Apple ships raw AAC-LC access units (no ADTS); the decoder needs the
        // 2-byte AudioSpecificConfig: objectType(5)=2 | freqIndex(4) | channels(4).
        const int asc = ((2 << 11) | (aacFreqIndex(rate) << 7) | (channels << 3)) & 0xffff;
        _ctx->extradata = (uint8_t *)av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
        _ctx->extradata[0] = (uint8_t)(asc >> 8);
        _ctx->extradata[1] = (uint8_t)(asc & 0xff);
        _ctx->extradata_size = 2;
    }
    else // OPUS: a minimal OpusHead so the decoder knows channels/rate.
    {
        static const uint8_t head[19] = {'O',  'p',  'u',  's',  'H', 'e', 'a',  'd',  0x01, 0x01,
                                         0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00};
        _ctx->extradata = (uint8_t *)av_mallocz(sizeof(head) + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(_ctx->extradata, head, sizeof(head));
        _ctx->extradata[9] = (uint8_t)channels; // channel count
        _ctx->extradata_size = sizeof(head);
    }

    if (avcodec_open2(_ctx, dec, nullptr) < 0)
    {
        log_e("[cp-audio] failed to open %s decoder", codec == Codec::AacLc ? "AAC" : "OPUS");
        avcodec_free_context(&_ctx);
        return false;
    }
    _pkt = av_packet_alloc();
    _frame = av_frame_alloc();
    return _pkt && _frame;
}

void Decoder::appendFrame(std::vector<int16_t> &out)
{
    const int n = _frame->nb_samples;
    const int ch = _frame->ch_layout.nb_channels;
    const AVSampleFormat fmt = (AVSampleFormat)_frame->format;
    const bool planar = av_sample_fmt_is_planar(fmt);

    auto emit = [&](int16_t s) { out.push_back(s); };
    for (int i = 0; i < n; i++)
    {
        for (int c = 0; c < ch; c++)
        {
            int16_t s = 0;
            switch (fmt)
            {
            case AV_SAMPLE_FMT_FLTP: s = f2s16(((const float *)_frame->data[c])[i]); break;
            case AV_SAMPLE_FMT_FLT:  s = f2s16(((const float *)_frame->data[0])[i * ch + c]); break;
            case AV_SAMPLE_FMT_S16P: s = ((const int16_t *)_frame->data[c])[i]; break;
            case AV_SAMPLE_FMT_S16:  s = ((const int16_t *)_frame->data[0])[i * ch + c]; break;
            default:
                // Fallback for other planar/packed layouts.
                if (planar)
                    s = f2s16(((const float *)_frame->data[c])[i]);
                else
                    s = ((const int16_t *)_frame->data[0])[i * ch + c];
                break;
            }
            emit(s);
        }
        if (_upmix && ch == 1) // duplicate mono -> stereo (second channel)
            emit(out.back());
    }
}

bool Decoder::decode(const uint8_t *au, int len, std::vector<int16_t> &out)
{
    if (_codec == Codec::Pcm)
    {
        // LPCM: wire samples are 16-bit big-endian; swap to native S16.
        const int samples = len / 2;
        out.reserve(out.size() + (size_t)samples * (_upmix ? 2 : 1));
        for (int i = 0; i < samples; i++)
        {
            int16_t s = (int16_t)((au[i * 2] << 8) | au[i * 2 + 1]);
            out.push_back(s);
            if (_upmix)
                out.push_back(s);
        }
        return true;
    }

    _pkt->data = (uint8_t *)au;
    _pkt->size = len;
    if (avcodec_send_packet(_ctx, _pkt) < 0)
        return false;
    bool any = false;
    while (avcodec_receive_frame(_ctx, _frame) == 0)
    {
        appendFrame(out);
        any = true;
    }
    return any;
}

} // namespace cp_audio
