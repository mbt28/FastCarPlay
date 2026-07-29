#ifndef SRC_PROTOCOL_CP_CP_AUDIO
#define SRC_PROTOCOL_CP_CP_AUDIO

// CarPlay audio decode. Each audio stream carries compressed access units
// (AAC-LC for buffered media, OPUS for nav/speech/telephony) or raw LPCM, one
// unit per RTP packet. This turns a decrypted access unit into S16 interleaved
// PCM (native endian) for the app's PcmAudio, decoding with libavcodec (already
// linked) rather than GStreamer -- no audio-server dependency, so the F1C's
// ALSA-only output path is unaffected. Mono is upmixed to stereo so the PCM maps
// onto a rate/channel config PcmAudio understands. Port of the LIVI decode step
// (cp/stack/rtpAudioDecoder.ts), libavcodec instead of gst.

#include <cstdint>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace cp_audio
{
using Bytes = std::vector<uint8_t>;

enum class Codec
{
    AacLc, // raw AAC-LC access units (AudioSpecificConfig as extradata)
    Opus,  // raw OPUS packets (48k)
    Pcm,   // LPCM passthrough: wire is S16 big-endian, swap to native
};

class Decoder
{
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    // `rate`/`channels` describe the stream the phone negotiated. `upmixToStereo`
    // duplicates a mono decode to stereo (OPUS nav is 48k mono, which PcmAudio has
    // no config for). Returns false on failure (codec missing, open failed).
    bool init(Codec codec, int rate, int channels, bool upmixToStereo);

    // Decode one access unit into S16 interleaved PCM (appended to `out`).
    bool decode(const uint8_t *au, int len, std::vector<int16_t> &out);

    int outRate() const { return _outRate; }
    int outChannels() const { return _outChannels; }

private:
    void appendFrame(std::vector<int16_t> &out);

    Codec _codec = Codec::Pcm;
    int _srcChannels = 2;
    int _outRate = 48000;
    int _outChannels = 2;
    bool _upmix = false;
    AVCodecContext *_ctx = nullptr;
    AVPacket *_pkt = nullptr;
    AVFrame *_frame = nullptr;
};

} // namespace cp_audio

#endif /* SRC_PROTOCOL_CP_CP_AUDIO */
