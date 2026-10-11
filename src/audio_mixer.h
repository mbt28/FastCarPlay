#ifndef SRC_AUDIO_MIXER
#define SRC_AUDIO_MIXER

#include <atomic>
#include <memory>
#include <thread>

#include "struct/atomic_queue.h"
#include "protocol/message.h"

// Forward-declare so every translation unit including this header does not pull
// the whole of alsa/asoundlib.h.
typedef struct _snd_pcm snd_pcm_t;

// Direct-ALSA playback engine: ONE thread owns hw:0,0 and mixes the two app
// audio queues (main = buffered media/music, aux = voice/nav) in software.
//
// This replaces the former pair of SDL queued-audio devices summed by an ALSA
// dmix: SDL's per-device pump threads and the app's polling burned most of the
// CPU during a call (measured ~84% + ~57% of the core) while the actual DSP is
// trivial integer work. A blocking snd_pcm_writei() sleeps on the codec clock,
// so this thread wakes exactly once per period and idles at ~0%.
//
// Everything is integer (ARM926, no FPU): a Q16 phase-accumulator linear
// resampler brings any source rate to the fixed 48 kHz hardware rate (the F1C
// codec is 48k-native), mono is duplicated to stereo, the aux voice ducks the
// main music through the same Q15 ramp the old PcmAudio used, and the mix is a
// saturating int32 sum.
class AudioMixer
{
public:
    AudioMixer();
    ~AudioMixer();

    void start(AtomicQueue<Message> *main, AtomicQueue<Message> *aux);
    void stop();

private:
    // One consumed stream: the current segment being resampled plus the
    // interpolation state that carries across segment and period boundaries.
    struct Source
    {
        AtomicQueue<Message> *q = nullptr;
        std::unique_ptr<Message> seg;
        const int16_t *segData = nullptr;
        int segFrames = 0; // source frames in the segment
        int segPos = 0;    // consumed source frames
        int rate = 0;
        int channels = 0;
        uint32_t step = 0;  // Q16 source-frames per output frame
        uint32_t phase = 0; // Q16 position between prev and next
        int16_t prevL = 0, prevR = 0;
        int16_t nextL = 0, nextR = 0;
        bool primed = false; // prev/next hold valid samples
        bool live = false;   // consuming (prefill satisfied)
        int dryPeriods = 0;  // consecutive periods with nothing to consume
        uint32_t zeroAcc = 0; // |sample| OR-accumulator for silence detection
    };

    void loop();
    bool openDevice();
    void closeDevice();
    // Pop + decode the next segment header; false when the queue is empty.
    bool nextSegment(Source &s);
    // One source frame (L/R native int16); false when the source ran dry.
    bool fetchFrame(Source &s, int16_t &l, int16_t &r);
    // Resample/upmix `frames` output frames into the int32 accumulator with a
    // per-frame Q15 gain ramp. Returns the frames actually rendered (the rest
    // stay silent when the source runs dry mid-period).
    int renderSource(Source &s, int32_t *mix, int frames, int32_t &volQ15, int32_t targetQ15);
    // Drop stale aux segments past the voice latency cap.
    void capAuxQueue();

    std::thread _thread;
    std::atomic<bool> _active{false};
    snd_pcm_t *_pcm = nullptr;
    Source _main, _aux;
    int32_t _mainVolQ15 = 32768;  // ducked by aux voice
    int32_t _mainTargetQ15 = 32768;
    int _auxSilentPeriods = 0;
};

#endif /* SRC_AUDIO_MIXER */
