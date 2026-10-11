#include "pcm_audio.h"
#include "common/functions.h"
#include "protocol/protocol_const.h"
#include "settings.h"
#include "common/logger.h"
#include <time.h>

// Add sample size (buffer size in samples) to ChannelConfig. const -> .rodata.
// The scale (3rd field) multiplies audio-buffer-samples into the SDL period, so
// it sets the per-format buffer depth. Media (type 4, 48k stereo) gets a deep
// buffer -- it is latency-insensitive and is where the CPU-starvation underruns
// bite; voice (mono) stays shallower to keep call latency down.
const ChannelConfig PcmAudio::_configTable[] = {
    {8000, 1, 1},  // type = 3
    {48000, 2, 8}, // type = 4  media (48k stereo): deep buffer vs CPU-spike underruns
    {16000, 1, 2}, // type = 5  voice
    {24000, 1, 2}, // type = 6  voice
    {16000, 2, 2}, // type = 7
    {48000, 1, 2}, // type = 8  voice (wired 48k mono PCM)
};

PcmAudio::PcmAudio(const char *name) : _name("default"),
                                       _fader(nullptr),
                                       _playing(false),
                                       _active(false),
                                       _fade(false),
                                       _config({0, 0, 0}),
                                       _volQ15(32768),
                                       _fadedQ15((int32_t)(Settings::audioFade * 32768.0f + 0.5f))
{
    if (name && strlen(name) > 0)
        _name = name;
    log_v("Created %s", _name.c_str());
}

PcmAudio::~PcmAudio()
{
    stop();
    if (_thread.joinable())
        _thread.join();
    log_v("Destroyed %s", _name.c_str());
}

void PcmAudio::start(AtomicQueue<Message> *data, PcmAudio *fader)
{
    if (_active)
        stop();
    log_v("Starting %s", _name.c_str());
    _fader = fader;
    _data = data;
    _active = true;
    _thread = std::thread(&PcmAudio::loop, this);
}

void PcmAudio::stop()
{
    if (!_active)
        return;
    log_v("Stopping %s", _name.c_str());
    _active = false;
    _data->notify();
}

ChannelConfig PcmAudio::getConfig(const Message *msg)
{
    uint8_t type = 0;
    if (msg)
    {
        type = msg->getInt(OFFSET_AUDIO_FORMAT);
    }

    if (type >= 3 && type <= 8)
        return _configTable[type - 3];
    // Default: 44100Hz, stereo, 4096 samples
    return {44100, 2, 4};
}

void PcmAudio::fade(bool enable)
{
    _fade.store(enable);
    if (!_playing)
        _volQ15 = enable ? _fadedQ15 : 32768;
}

// One pass over the segment: ramp the gain (per frame, so the fade lasts the
// same wall-clock time at any channel count), scale each sample, and report
// whether the whole segment is zero -- folding what used to be a second
// uint64 walk in isZero() into this loop. All integer: no soft-float.
bool PcmAudio::fadeAndScan(uint8_t *data, int32_t length, int channels)
{
    const bool fade = _fade.load();
    int16_t *buf = reinterpret_cast<int16_t *>(data);
    const int samples = length / 2;
    if (channels < 1)
        channels = 1;

    // No fading needed: just scan for silence.
    if (!fade && _volQ15 >= 32768)
    {
        uint32_t acc = 0;
        for (int i = 0; i < samples; i++)
            acc |= (uint16_t)buf[i];
        return acc == 0;
    }

    uint32_t acc = 0;
    int inFrame = 0;
    for (int i = 0; i < samples; i++)
    {
        if (inFrame == 0) // advance the ramp once per frame
        {
            if (fade)
            {
                _volQ15 -= FADE_OUT_STEP_Q15;
                if (_volQ15 < _fadedQ15) _volQ15 = _fadedQ15;
            }
            else
            {
                _volQ15 += FADE_IN_STEP_Q15;
                if (_volQ15 > 32768) _volQ15 = 32768;
            }
        }
        if (_volQ15 < 32768)
            buf[i] = (int16_t)(((int32_t)buf[i] * _volQ15) >> 15);
        acc |= (uint16_t)buf[i];
        if (++inFrame >= channels)
            inFrame = 0;
    }
    return acc == 0;
}

void PcmAudio::play(SDL_AudioDeviceID device, ChannelConfig config, int32_t segmentSize)
{
    uint8_t zeroSegments = 0;
    bool nonZero = false;
    int overCap = 0;

    int prefill = config.channels == 1 ? Settings::audioDelayCall : Settings::audioDelay;
    int segmentTimeMs = 1000.0 * segmentSize / (config.rate * config.channels * 2.0);
    int waitTimeMs = (prefill + 1) * segmentTimeMs;
    log_i("Prepare to play %s %dkHz %s chunk %d ~%dms prefill %d ~%dms", _name.c_str(),
          config.rate,
          (config.channels == 2 ? "stereo" : "mono"),
          segmentSize,
          segmentTimeMs,
          prefill,
          waitTimeMs);

    if (!_data->waitFor(_active, AUDIO_RESET_SECONDS * 1000, prefill))
    {
        _data->clear();
        log_w("Not enough data to play %s %dkHz %s chunk %d ~%dms prefill %d ~%dms",
              _name.c_str(),
              config.rate,
              (config.channels == 2 ? "stereo" : "mono"),
              segmentSize,
              segmentTimeMs,
              prefill,
              waitTimeMs);
        return;
    }

    if (_fader && !_fader->faded())
    {
        SDL_Delay(Settings::audioAuxDelay);
    }

    while (_active)
    {
        std::unique_ptr<Message> segment = _data->pop();
        if (!segment)
            return;
        if (config != getConfig(segment.get()))
            return;

        // Fade + silence-detect in one integer pass over the segment.
        const bool silent = fadeAndScan(segment->data(), segment->length(), config.channels);

        SDL_QueueAudio(device, segment->data(), segment->length());

        // Voice: keep the queue near the live edge so a call is not heard late.
        // Only a SUSTAINED overshoot of the latency cap is a real lag worth a
        // flush -- a delivery burst overshoots briefly and drains on its own,
        // and flushing per overshoot discarded most of the call audio. After
        // the flush, re-queue the current segment so playback resumes at the
        // live edge with no silence gap. Media is not capped.
        if (_fader && _playing)
        {
            const Uint32 cap = (Uint32)((int64_t)config.rate * config.channels * 2 *
                                        Settings::audioMaxLatencyMs / 1000);
            if (SDL_GetQueuedAudioSize(device) > cap)
            {
                if (++overCap >= VOICE_OVER_CAP_SEGMENTS)
                {
                    log_d("Audio %s over %dms for %d segments, flush to live edge",
                          _name.c_str(), (int)Settings::audioMaxLatencyMs, overCap);
                    SDL_ClearQueuedAudio(device);
                    SDL_QueueAudio(device, segment->data(), segment->length());
                    overCap = 0;
                }
            }
            else
                overCap = 0;
        }

        if (!_playing && prefill-- <= 0)
        {
            log_d("Start playing %s %dkHz %s",
                  _name.c_str(),
                  config.rate,
                  (config.channels == 2 ? "stereo" : "mono"));
            SDL_PauseAudioDevice(device, 0);
            _playing = true;
        }

        if (_fader)
        {
            if (silent)
            {
                if (nonZero && ++zeroSegments == FADE_ZERO_SEGMENTS)
                {
                    log_d("Audio %s is zeroes, fade other channel in", _name.c_str());
                    _fader->fade(false);
                }
            }
            else
            {
                nonZero = true;
                zeroSegments = 0;
                _fader->fade(true);
            }
        }

        // Wait for the next chunk before looping back to pop(). Note waitFor()
        // returns true on BOTH new data and timeout while active, so we must loop
        // until data is actually queued (has(1)). A bursty source -- wireless
        // CarPlay buffers ~1s of media ahead and ships it in bursts, with gaps
        // longer than one chunk -- would otherwise leave pop() empty during a
        // gap, and loop() then pauses/re-opens the device, chopping the audio
        // still queued in SDL. So once we're playing, ride out the gap on SDL's
        // own queue and only give up when it is nearly dry (a real underrun / end
        // of stream). Steady sources fall straight through on the first wait.
        const Uint32 lowWater = (Uint32)(config.rate * config.channels * 2 / 20); // ~50ms
        while (_playing && !_data->has(1))
        {
            _data->waitFor(_active, 50); // short poll: catch the next burst promptly
            if (!_active)
                return;
            if (!_data->has(1) && SDL_GetQueuedAudioSize(device) <= lowWater)
                return; // genuinely dry
        }
        if (!_playing && !_data->waitFor(_active, waitTimeMs))
            return; // still prefilling: original behaviour
    }
}

void PcmAudio::loop()
{
    std::string threadName = "audio-" + _name;
    setThreadName(threadName.c_str());
    // Elevate playback above the SCHED_OTHER video decode (same level as the mic
    // capture/uplink thread): on this single-CPU board the H.264 decoder would
    // otherwise starve the DAC-refill loop for tens of ms, underrunning the
    // caller/music audio. Both audio threads at FIFO 10 coexist above video and
    // below the USB transport's realtime (FIFO 50). Tiny CPU, so video is fine.
    setThreadPriority(ThreadPriority::High);

    log_d("Started thread %s", _name.c_str());

    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec;

    time_t playEnd = time(NULL);
    while (_active)
    {
        // Acquire the next segment. If we are holding an open (paused) device
        // from the previous burst, wait only the idle-grace window: if nothing
        // arrives, release the codec + its dmix client (frees CPU on this
        // CPU-tight board). Audio that keeps flowing within the window rides the
        // still-open handle, so there is no reopen churn mid-call.
        if (device != 0)
        {
            _data->waitFor(_active, Settings::audioIdleHoldMs);
            if (!_active)
                break;
            if (!_data->has(1))
            {
                SDL_PauseAudioDevice(device, 1);
                SDL_ClearQueuedAudio(device);
                SDL_CloseAudioDevice(device);
                device = 0;
                _config = ChannelConfig{};
                playEnd = time(NULL);
                continue;
            }
        }
        else if (!_data->wait(_active))
            break;

        const Message *segment = _data->peek();
        if (!segment)
            continue;

        ChannelConfig config = getConfig(segment);
        if (_config != config)
        {
            if (device != 0)
            {
                SDL_PauseAudioDevice(device, 1);
                SDL_ClearQueuedAudio(device);
                SDL_CloseAudioDevice(device);
                device = 0;
            }

            // Configure new spec
            SDL_zero(spec);
            spec.freq = config.rate;
            spec.format = AUDIO_S16SYS;
            spec.channels = config.channels;
            // Voice (aux, _fader set) opens a small buffer for low call latency;
            // media (main) keeps the deep audioBuffer*scale buffer for smoothness.
            spec.samples = _fader ? (Uint16)Settings::audioVoiceBuffer
                                  : (Uint16)(Settings::audioBuffer * config.scale);
            spec.callback = nullptr;
            spec.userdata = nullptr;

            // Passing obtained + ALLOW_FREQUENCY_CHANGE: if the codec cannot
            // deliver this rate natively SDL would otherwise interpose a CPU
            // resampler on every queued buffer. We'd rather know and let the
            // device pick its own rate (SDL_QueueAudio still accepts our data).
            SDL_AudioSpec obtained;
            device = SDL_OpenAudioDevice(nullptr, 0, &spec, &obtained,
                                         SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
            if (device != 0 && obtained.freq != spec.freq)
                log_w("Audio %s: asked %dHz, device gave %dHz (SDL resamples)",
                      _name.c_str(), spec.freq, obtained.freq);
            if (device == 0)
            {
                log_w("Failed to open audio %s %dkHz %s samples %d > %s",
                      _name.c_str(),
                      config.rate,
                      (config.channels == 2 ? "stereo" : "mono"),
                      (int)spec.samples, SDL_GetError());
                SDL_Delay(100);
                continue;
            }
            _config = config;
        }

        if (device != 0 && difftime(time(NULL), playEnd) > AUDIO_RESET_SECONDS)
            SDL_ClearQueuedAudio(device);

        if (_fader)
            _fader->fade(true);
        play(device, config, segment->length());
        _playing = false;
        if (_fader)
            _fader->fade(false);
        // Hold the device open + paused across the gap (no reopen churn); the
        // loop top releases it after the idle-grace window if audio really
        // stopped, so an idle stream does not keep a dmix client (and its CPU)
        // alive. dmix mixes concurrently, so holding it open costs no EBUSY.
        SDL_PauseAudioDevice(device, 1);
        SDL_ClearQueuedAudio(device);
        playEnd = time(NULL);
    }

    if (device != 0)
    {
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
    }

    log_v("Stopped thread %s", _name.c_str());
}
