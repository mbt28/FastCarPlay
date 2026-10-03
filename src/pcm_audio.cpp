#include "pcm_audio.h"
#include "common/functions.h"
#include "protocol/protocol_const.h"
#include "settings.h"
#include "common/logger.h"
#include <time.h>

// Add sample size (buffer size in samples) to ChannelConfig. const -> .rodata.
const ChannelConfig PcmAudio::_configTable[] = {
    {8000, 1, 1},  // type = 3, ~256ms
    {48000, 2, 4}, // type = 4, ~170ms
    {16000, 1, 2}, // type = 5, ~256ms
    {24000, 1, 2}, // type = 6, ~170ms
    {16000, 2, 2}, // type = 7, ~256ms
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

    if (type >= 3 && type <= 7)
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

    log_d("Started thread %s", _name.c_str());

    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec spec;

    time_t playEnd = time(NULL);
    while (_data->wait(_active))
    {
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
            spec.samples = Settings::audioBuffer * config.scale;
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
                      Settings::audioBuffer * config.scale, SDL_GetError());
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
        SDL_PauseAudioDevice(device, 1);
        // Release the hardware while idle. The F1C200s codec has ONE playback
        // stream and alsa-lib's default device has no software mixer, so a
        // paused-but-open "main" made every "aux" open (calls, Siri, nav) fail
        // with EBUSY (seen 2026-10-03: no call audio). Reopening on the next
        // segment costs the codec's power-up delay, which the prefill absorbs.
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
        device = 0;
        _config = ChannelConfig{}; // forces a fresh open for the next segment
        playEnd = time(NULL);
        log_d("Stop playing %s %dkHz %s",
              _name.c_str(),
              config.rate,
              (config.channels == 2 ? "stereo" : "mono"));
    }

    if (device != 0)
    {
        SDL_ClearQueuedAudio(device);
        SDL_CloseAudioDevice(device);
    }

    log_v("Stopped thread %s", _name.c_str());
}
