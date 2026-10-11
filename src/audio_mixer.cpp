#include "audio_mixer.h"

#include <alsa/asoundlib.h>

#include "common/functions.h"
#include "common/logger.h"
#include "protocol/protocol_const.h"
#include "settings.h"

// Fixed hardware shape: the F1C codec is 48k-native (44.1k family unsupported)
// and has a single playback stream. Period/buffer mirror the dmix slave config
// that the kernel driver was validated with (1024/4096 frames = ~21ms/~85ms).
#define MIX_RATE 48000
#define MIX_CHANNELS 2
#define MIX_PERIOD 1024
#define MIX_BUFFER 4096
#define MIX_PERIOD_MS (1000 * MIX_PERIOD / MIX_RATE)

// Q15 duck ramp, one step per output frame (same rates as the old PcmAudio).
#define FADE_IN_STEP_Q15 1
#define FADE_OUT_STEP_Q15 3
// Aux silent this many periods before music ramps back (~85ms, matching the
// old 10-segment release at 7.5ms voice segments).
#define AUX_SILENT_RELEASE_PERIODS 4
// A live source dry this long reverts to needing a prefill (burst gaps shorter
// than this resume instantly on the hw buffer).
#define DRY_RELEASE_PERIODS 24

namespace
{
struct Fmt
{
    int rate;
    int channels;
};
// Format word at segment offset 0 -> rate/channels. CarPlay encodes the exact
// negotiated shape as (rate << 4) | channels (always > 0xFFFF); the carlinkit
// dongle and AA paths still use the legacy small enum, where anything unknown
// is the dongle default of 44.1k stereo.
Fmt fmtOf(const Message *m)
{
    const uint32_t v = m ? (uint32_t)m->getInt(OFFSET_AUDIO_FORMAT) : 0;
    if (v > 0xFFFF)
    {
        const int ch = (int)(v & 0xF);
        return {(int)(v >> 4), (ch == 1 || ch == 2) ? ch : 2};
    }
    switch (v)
    {
        case 3: return {8000, 1};
        case 4: return {48000, 2};
        case 5: return {16000, 1};
        case 6: return {24000, 1};
        case 7: return {16000, 2};
        default: return {44100, 2};
    }
}
} // namespace

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer()
{
    stop();
    if (_thread.joinable())
        _thread.join();
}

void AudioMixer::start(AtomicQueue<Message> *main, AtomicQueue<Message> *aux)
{
    if (_active)
        stop();
    if (_thread.joinable())
        _thread.join();
    _main = Source{};
    _aux = Source{};
    _main.q = main;
    _aux.q = aux;
    _active = true;
    _thread = std::thread(&AudioMixer::loop, this);
}

void AudioMixer::stop()
{
    if (!_active.exchange(false))
        return;
    if (_main.q)
        _main.q->notify();
    if (_aux.q)
        _aux.q->notify();
}

bool AudioMixer::openDevice()
{
    const std::string &dev = Settings::alsaDevice.value;
    int err = snd_pcm_open(&_pcm, dev.c_str(), SND_PCM_STREAM_PLAYBACK, 0); // blocking
    if (err < 0)
    {
        log_w("[mixer] open %s failed: %s", dev.c_str(), snd_strerror(err));
        _pcm = nullptr;
        return false;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(_pcm, hw);
    snd_pcm_hw_params_set_access(_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(_pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(_pcm, hw, MIX_CHANNELS);
    unsigned rate = MIX_RATE;
    snd_pcm_hw_params_set_rate_near(_pcm, hw, &rate, nullptr);
    snd_pcm_uframes_t period = MIX_PERIOD, buffer = MIX_BUFFER;
    snd_pcm_hw_params_set_period_size_near(_pcm, hw, &period, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(_pcm, hw, &buffer);
    err = snd_pcm_hw_params(_pcm, hw);
    if (err < 0 || rate != MIX_RATE)
    {
        log_w("[mixer] hw params failed (%s, rate %u)", snd_strerror(err), rate);
        snd_pcm_close(_pcm);
        _pcm = nullptr;
        return false;
    }

    // Start on the second period so the first write never races the DAC; wake
    // this thread only when a full period fits (one wakeup per period).
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(_pcm, sw);
    snd_pcm_sw_params_set_start_threshold(_pcm, sw, 2 * MIX_PERIOD);
    snd_pcm_sw_params_set_avail_min(_pcm, sw, MIX_PERIOD);
    snd_pcm_sw_params(_pcm, sw);

    log_i("[mixer] %s open: %u Hz %dch S16, period %lu, buffer %lu", dev.c_str(), rate,
          MIX_CHANNELS, (unsigned long)period, (unsigned long)buffer);
    return true;
}

void AudioMixer::closeDevice()
{
    if (!_pcm)
        return;
    snd_pcm_drop(_pcm);
    snd_pcm_close(_pcm);
    _pcm = nullptr;
    log_d("[mixer] device released (idle)");
}

bool AudioMixer::nextSegment(Source &s)
{
    s.seg = s.q->pop();
    if (!s.seg)
        return false;
    const Fmt f = fmtOf(s.seg.get());
    if (f.rate != s.rate || f.channels != s.channels)
    {
        s.rate = f.rate;
        s.channels = f.channels;
        s.step = (uint32_t)(((uint64_t)f.rate << 16) / MIX_RATE);
        s.primed = false; // format change: re-prime the interpolator
    }
    s.segData = reinterpret_cast<const int16_t *>(s.seg->data());
    s.segFrames = s.seg->length() / (2 * s.channels);
    s.segPos = 0;
    return s.segData != nullptr;
}

bool AudioMixer::fetchFrame(Source &s, int16_t &l, int16_t &r)
{
    while (!s.seg || s.segPos >= s.segFrames)
        if (!nextSegment(s))
            return false;
    const int16_t *p = s.segData + (size_t)s.segPos * s.channels;
    l = p[0];
    r = (s.channels == 2) ? p[1] : p[0];
    s.segPos++;
    s.zeroAcc |= (uint16_t)l | (uint16_t)r;
    return true;
}

int AudioMixer::renderSource(Source &s, int32_t *mix, int frames, int32_t &volQ15, int32_t targetQ15)
{
    for (int i = 0; i < frames; i++)
    {
        if (volQ15 < targetQ15)
        {
            volQ15 += FADE_IN_STEP_Q15;
            if (volQ15 > targetQ15) volQ15 = targetQ15;
        }
        else if (volQ15 > targetQ15)
        {
            volQ15 -= FADE_OUT_STEP_Q15;
            if (volQ15 < targetQ15) volQ15 = targetQ15;
        }

        if (!s.primed)
        {
            if (!fetchFrame(s, s.prevL, s.prevR))
                return i;
            if (!fetchFrame(s, s.nextL, s.nextR))
            {
                s.nextL = s.prevL;
                s.nextR = s.prevR;
            }
            s.phase = 0;
            s.primed = true;
        }

        s.phase += s.step;
        while (s.phase >= 65536)
        {
            s.phase -= 65536;
            s.prevL = s.nextL;
            s.prevR = s.nextR;
            if (!fetchFrame(s, s.nextL, s.nextR))
            {
                // Dry mid-run: emit what we have and re-prime on resume.
                s.primed = false;
                s.phase = 0;
                break;
            }
        }

        // Linear interpolation; smull keeps the 32x32 product exact.
        const int32_t l = s.prevL + (int32_t)(((int64_t)(s.nextL - s.prevL) * s.phase) >> 16);
        const int32_t r = s.prevR + (int32_t)(((int64_t)(s.nextR - s.prevR) * s.phase) >> 16);
        mix[2 * i] += (l * volQ15) >> 15;
        mix[2 * i + 1] += (r * volQ15) >> 15;
    }
    return frames;
}

void AudioMixer::capAuxQueue()
{
    const Message *head = _aux.q->peek();
    if (!head)
        return;
    const Fmt f = fmtOf(head);
    const int segMs = (int)(1000LL * (head->length() / (2 * f.channels)) / (f.rate ? f.rate : 48000));
    if (segMs <= 0)
        return;
    // Keep the voice near the live edge: drop the OLDEST segments once the
    // queued backlog exceeds the latency cap (the bounded queue + the small hw
    // buffer already limit the worst case; this trims a sustained lead).
    while ((int)_aux.q->count() * segMs > Settings::audioMaxLatencyMs && _aux.q->count() > 1)
        _aux.q->pop();
}

void AudioMixer::loop()
{
    setThreadName("audio-mixer");
    // Above the SCHED_OTHER video decoder, below the USB transport (FIFO 50):
    // this thread sleeps in snd_pcm_writei and must never be starved into a
    // DAC underrun by a decode burst.
    setThreadPriority(ThreadPriority::High);
    log_d("[mixer] thread started");

    const int32_t fadeFloorQ15 = (int32_t)(Settings::audioFade * 32768.0f + 0.5f);
    int32_t mix[MIX_PERIOD * MIX_CHANNELS];
    int16_t out[MIX_PERIOD * MIX_CHANNELS];
    int idlePeriods = 0;

    while (_active)
    {
        if (!_pcm)
        {
            // Idle: no device, block until either queue has data. Waiting on
            // the aux queue keeps voice onset latency minimal; main (music)
            // start tolerates the 100ms poll granularity.
            if (!_main.q->has(1) && !_aux.q->has(1))
            {
                _aux.q->waitFor(_active, 100);
                if (!_active)
                    break;
                continue;
            }
            if (!openDevice())
            {
                _aux.q->waitFor(_active, 500);
                continue;
            }
            _mainVolQ15 = 32768;
            _mainTargetQ15 = 32768;
            _auxSilentPeriods = AUX_SILENT_RELEASE_PERIODS;
            idlePeriods = 0;
        }

        // Source liveness: a dry source needs a short prefill before it starts
        // feeding (absorbs bursty delivery); a brief gap resumes instantly.
        for (Source *s : {&_main, &_aux})
        {
            if (!s->live)
            {
                const Message *head = s->q->peek();
                if (head)
                {
                    const int prefill = (fmtOf(head).channels == 1) ? Settings::audioDelayCall
                                                                    : Settings::audioDelay;
                    if (s->q->has((uint16_t)prefill))
                    {
                        s->live = true;
                        s->dryPeriods = 0;
                    }
                }
            }
        }

        capAuxQueue();

        memset(mix, 0, sizeof(mix));
        int mainRendered = 0, auxRendered = 0;
        _aux.zeroAcc = 0;
        if (_main.live)
            mainRendered = renderSource(_main, mix, MIX_PERIOD, _mainVolQ15, _mainTargetQ15);
        int32_t auxVol = 32768;
        if (_aux.live)
            auxRendered = renderSource(_aux, mix, MIX_PERIOD, auxVol, 32768);

        // Duck: audible voice pulls music down to the fade floor; after the
        // release window of silent/dry periods it ramps back up.
        if (auxRendered > 0 && _aux.zeroAcc != 0)
        {
            _mainTargetQ15 = fadeFloorQ15;
            _auxSilentPeriods = 0;
        }
        else if (++_auxSilentPeriods >= AUX_SILENT_RELEASE_PERIODS)
            _mainTargetQ15 = 32768;

        // Dry bookkeeping: a source that rendered nothing this period.
        for (Source *s : {&_main, &_aux})
        {
            const int rendered = (s == &_main) ? mainRendered : auxRendered;
            if (s->live && rendered == 0 && !s->q->has(1))
            {
                if (++s->dryPeriods >= DRY_RELEASE_PERIODS)
                {
                    s->live = false;
                    s->primed = false;
                }
            }
            else if (rendered > 0)
                s->dryPeriods = 0;
        }

        for (int i = 0; i < MIX_PERIOD * MIX_CHANNELS; i++)
        {
            const int32_t v = mix[i];
            out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }

        snd_pcm_sframes_t w = snd_pcm_writei(_pcm, out, MIX_PERIOD);
        if (w < 0)
        {
            w = snd_pcm_recover(_pcm, (int)w, 1);
            if (w < 0)
            {
                log_w("[mixer] write failed: %s -- reopening", snd_strerror((int)w));
                closeDevice();
                continue;
            }
            snd_pcm_writei(_pcm, out, MIX_PERIOD); // retry once after recover
        }

        // Release the codec after a real idle stretch (saves the period wakeups
        // and lets a future owner reconfigure the hw cleanly).
        if (mainRendered == 0 && auxRendered == 0 && !_main.q->has(1) && !_aux.q->has(1))
        {
            if (++idlePeriods * MIX_PERIOD_MS > Settings::audioIdleHoldMs)
            {
                closeDevice();
                _main.live = _aux.live = false;
                _main.primed = _aux.primed = false;
            }
        }
        else
            idlePeriods = 0;
    }

    closeDevice();
    log_d("[mixer] thread stopped");
}
