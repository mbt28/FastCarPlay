#ifndef SRC_PCM_AUDIO
#define SRC_PCM_AUDIO

#include <atomic>
#include <string>
#include <thread>

#include <SDL2/SDL.h>

#include "struct/atomic_queue.h"
#include "protocol/message.h"

// Fade ramp in Q15 (volume 0..32768). The step is per audio FRAME, so a
// stereo and a mono stream fade over the same wall-clock time. These match
// the old float rates (0.00001 / 0.0001 of full scale) closely enough:
// ~0.33 Q15/frame in, ~3.3 out. The whole fade path is integer now -- the
// ARM926 has no FPU, so the old float-per-sample version cost ~10 libgcc
// soft-float calls on every single sample.
#define FADE_IN_STEP_Q15 1
#define FADE_OUT_STEP_Q15 3
#define FADE_ZERO_SEGMENTS 10
#define AUDIO_RESET_SECONDS 5

struct ChannelConfig
{
    int rate;
    uint8_t channels;
    uint8_t scale;

    bool operator==(ChannelConfig const &other) const
    {
        return rate == other.rate && channels == other.channels;
    }

    bool operator!=(ChannelConfig const &other) const
    {
        return !(*this == other);
    }
};

class PcmAudio
{
public:
    PcmAudio(const char *name = "");
    ~PcmAudio();

    // Start playing raw PCM data from queue
    void start(AtomicQueue<Message> *data, PcmAudio *fader = nullptr);
    void stop();

private:
    static ChannelConfig getConfig(const Message *msg);
    static const ChannelConfig _configTable[];

    void fade(bool enble);
    void loop();
    void play(SDL_AudioDeviceID device, ChannelConfig config, int32_t segmentSize);
    // Apply the fade ramp (and the zero test for the paired channel) in a
    // single pass over the segment. `channels` lets the ramp step once per
    // frame rather than once per sample. Returns true if every sample is zero.
    bool fadeAndScan(uint8_t *data, int32_t length, int channels);
    bool faded() const { return _volQ15 <= _fadedQ15; }

    std::string _name;
    PcmAudio *_fader;
    std::thread _thread;
    std::atomic<bool> _playing;
    std::atomic<bool> _active;
    std::atomic<bool> _fade;
    ChannelConfig _config;
    AtomicQueue<Message> *_data;
    int32_t _volQ15;    // current gain, Q15 (32768 = unity)
    int32_t _fadedQ15;  // floor the fade-out ramps down to
};

#endif /* SRC_PCM_AUDIO */
