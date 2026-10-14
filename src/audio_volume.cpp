#include "audio_volume.h"

#include <alsa/asoundlib.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "common/clock.h"
#include "common/logger.h"
#include "settings.h"

namespace
{
// How "auto" picks the control that sets the volume. Inputs mixed into the
// output ("Mic", "Line In"...) have playback volumes too, so nothing outside
// these lists is guessed: a card with none of them gets the software gain.
// 1. A control in front of every output.
constexpr const char *SHARED[] = {"Master", "PCM"};
// 2. A card's only output amplifier (the F1C codec's "Headphone"). With
//    several -- Speaker and Headphone, say -- none of them is the volume.
constexpr const char *OUTPUTS[] = {"Headphone", "HP",      "Line Out", "Lineout",
                                   "OUT",       "Speaker", "Earpiece"};
// 3. A digital volume feeding every output.
constexpr const char *DIGITAL[] = {"Digital", "DAC", "Playback"};

// ALSA PCM aliases whose first positional argument is the card (alsa-lib's
// conf/pcm/*.conf).
constexpr const char *BY_CARD[] = {"hw",         "plughw",     "sysdefault", "default",
                                   "front",      "rear",       "center_lfe", "side",
                                   "surround21", "surround40", "surround41", "surround50",
                                   "surround51", "surround71", "iec958",     "spdif",
                                   "hdmi",       "dmix",       "dsnoop"};

// A changed level is saved once it has been left alone this long.
constexpr uint32_t SAVE_AFTER_MS = 2000;

// dB ranges up to this wide (0.01 dB units) map linearly; wider ones by
// amplitude, cubed -- alsamixer's and PulseAudio's rule, so a step sounds
// about the same anywhere on the scale.
constexpr long LINEAR_DB_RANGE = 24 * 100;

bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

// The control device of the card an ALSA PCM name plays on: "hw:0,0",
// "plughw:1", "front:CARD=Device,DEV=0", "plug:hw:1" -> "hw:<card>";
// "default" when the name does not say (a plugin such as "pulse" has its
// volume on the default control device).
std::string mixerDeviceFor(std::string pcm)
{
    pcm.erase(std::remove(pcm.begin(), pcm.end(), '"'), pcm.end()); // plug:"hw:1,0"
    const std::size_t colon = pcm.find(':');
    if (colon == std::string::npos)
        return "default";
    const std::string kind = pcm.substr(0, colon);
    const std::string args = pcm.substr(colon + 1);
    const std::size_t named = args.find("CARD=");
    if (named != std::string::npos)
    {
        const std::size_t end = args.find(',', named);
        const std::string card =
            args.substr(named + 5, end == std::string::npos ? std::string::npos : end - (named + 5));
        return card.empty() ? std::string("default") : "hw:CARD=" + card;
    }
    if (args.find(':') != std::string::npos)
        return mixerDeviceFor(args); // a plugin over another PCM
    if (std::none_of(std::begin(BY_CARD), std::end(BY_CARD),
                     [&kind](const char *k) { return kind == k; }))
        return "default";
    const std::string card = args.substr(0, args.find(','));
    return card.empty() ? std::string("default") : "hw:" + card;
}

// level 1..100 -> the control, through its dB scale when it has one. Rounds
// up, so no level above 0 lands on a codec's mute step.
bool setLevel(snd_mixer_elem_t *e, int level)
{
    long min = 0, max = 0;
    if (level <= 0)
    {
        if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0)
            return false;
        return snd_mixer_selem_set_playback_volume_all(e, min) >= 0;
    }
    const double v = level / 100.0;
    if (snd_mixer_selem_get_playback_dB_range(e, &min, &max) < 0 || min >= max)
    {
        if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0)
            return false;
        return snd_mixer_selem_set_playback_volume_all(e, (long)std::ceil(v * (max - min)) + min) >= 0;
    }
    long db;
    if (max - min <= LINEAR_DB_RANGE)
        db = (long)std::lround(v * (max - min)) + min;
    else
    {
        double amp = v;
        if (min != SND_CTL_TLV_DB_GAIN_MUTE)
        {
            const double minAmp = std::pow(10.0, (min - max) / 6000.0);
            amp = amp * (1 - minAmp) + minAmp;
        }
        db = (long)std::lround(6000.0 * std::log10(amp)) + max;
    }
    return snd_mixer_selem_set_playback_dB_all(e, db, 1) >= 0;
}

// The control's current setting as a level (the inverse of setLevel); -1
// when it cannot be read.
int getLevel(snd_mixer_elem_t *e)
{
    long min = 0, max = 0, value = 0;
    double v;
    if (snd_mixer_selem_get_playback_dB_range(e, &min, &max) < 0 || min >= max)
    {
        if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0 || min >= max ||
            snd_mixer_selem_get_playback_volume(e, SND_MIXER_SCHN_FRONT_LEFT, &value) < 0)
            return -1;
        v = (double)(value - min) / (max - min);
    }
    else
    {
        if (snd_mixer_selem_get_playback_dB(e, SND_MIXER_SCHN_FRONT_LEFT, &value) < 0)
            return -1;
        if (max - min <= LINEAR_DB_RANGE)
            v = (double)(value - min) / (max - min);
        else
        {
            v = std::pow(10.0, (value - max) / 6000.0);
            if (min != SND_CTL_TLV_DB_GAIN_MUTE)
            {
                const double minAmp = std::pow(10.0, (min - max) / 6000.0);
                v = (v - minAmp) / (1 - minAmp);
            }
        }
    }
    return std::clamp((int)std::lround(v * 100.0), 0, 100);
}

snd_mixer_elem_t *findElem(snd_mixer_t *mixer, const char *name)
{
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, name);
    snd_mixer_elem_t *e = snd_mixer_find_selem(mixer, sid);
    return e && snd_mixer_selem_is_active(e) && snd_mixer_selem_has_playback_volume(e) ? e : nullptr;
}

// The control "auto" settles on, or null (see the lists above).
snd_mixer_elem_t *pickControl(snd_mixer_t *mixer)
{
    for (const char *name : SHARED)
        if (snd_mixer_elem_t *e = findElem(mixer, name))
            return e;
    snd_mixer_elem_t *only = nullptr;
    int outputs = 0;
    for (const char *name : OUTPUTS)
        if (snd_mixer_elem_t *e = findElem(mixer, name))
        {
            only = e;
            outputs++;
        }
    if (outputs == 1)
        return only;
    for (const char *name : DIGITAL)
        if (snd_mixer_elem_t *e = findElem(mixer, name))
            return e;
    return nullptr;
}
} // namespace

AudioVolume &AudioVolume::instance()
{
    static AudioVolume volume;
    return volume;
}

AudioVolume::~AudioVolume()
{
    closeMixer();
}

bool AudioVolume::openMixer()
{
    int err = snd_mixer_open(&_mixer, 0);
    if (err < 0)
    {
        _mixer = nullptr;
        if (!_quiet)
            log_w("volume: no mixer (%s) -- software gain", snd_strerror(err));
        _quiet = true;
        return false;
    }
    if ((err = snd_mixer_attach(_mixer, _device.c_str())) < 0 ||
        (err = snd_mixer_selem_register(_mixer, nullptr, nullptr)) < 0 ||
        (err = snd_mixer_load(_mixer)) < 0)
    {
        if (!_quiet)
            log_w("volume: mixer %s unavailable (%s) -- software gain", _device.c_str(),
                  snd_strerror(err));
        _quiet = true;
        closeMixer();
        return false;
    }
    const bool automatic = strcasecmp(_wanted.c_str(), "auto") == 0;
    snd_mixer_elem_t *e = automatic ? pickControl(_mixer) : findElem(_mixer, _wanted.c_str());
    if (e == nullptr)
    {
        if (!_quiet)
            log_w("volume: no %s playback control on %s -- software gain",
                  automatic ? "known" : ("'" + _wanted + "'").c_str(), _device.c_str());
        _quiet = true;
        closeMixer();
        return false;
    }
    _control = snd_mixer_selem_get_name(e);
    findControl();
    _quiet = false;
    return true;
}

void AudioVolume::closeMixer()
{
    if (_mixer != nullptr)
        snd_mixer_close(_mixer);
    _mixer = nullptr;
    _elem = nullptr;
    _hasSwitch = false;
    _muteStep = false;
}

// Looks the control up by name -- after events the old element may be gone
// (alsa-lib frees one whose controls were removed) -- and reads what it can
// do.
bool AudioVolume::findControl()
{
    _elem = _mixer != nullptr && !_control.empty() ? findElem(_mixer, _control.c_str()) : nullptr;
    _hasSwitch = false;
    _muteStep = false;
    if (_elem == nullptr)
        return false;
    _hasSwitch = snd_mixer_selem_has_playback_switch(_elem);
    long min = 0, max = 0;
    _muteStep = snd_mixer_selem_get_playback_dB_range(_elem, &min, &max) >= 0 &&
                min == SND_CTL_TLV_DB_GAIN_MUTE;
    return true;
}

void AudioVolume::start()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_started)
        return;
    _started = true;

    _wanted = Settings::volumeControl.value;
    _device = Settings::volumeDevice.value.empty() ? mixerDeviceFor(Settings::alsaDevice.value)
                                                   : Settings::volumeDevice.value;
    _control.clear();
    _quiet = false;
    if (strcasecmp(_wanted.c_str(), "software") != 0)
        openMixer();

    int level = Settings::volume.value;
    _ownLevel = level >= 0 && level <= 100;
    if (!_ownLevel)
    {
        // Never set from here yet: keep what the card is set to now -- and
        // do not write it back, which could round it a step up every start.
        level = _elem != nullptr ? getLevel(_elem) : -1;
        if (level < 0)
            level = 100;
    }
    _level.store(level, std::memory_order_relaxed);
    _muted.store(false, std::memory_order_relaxed);
    _atMuteStep = false;
    apply();
    _generation.fetch_add(1, std::memory_order_acq_rel);

    if (_elem != nullptr)
    {
        long min = 0, max = 0, rawMin = 0, rawMax = 0;
        if (snd_mixer_selem_get_playback_dB_range(_elem, &min, &max) >= 0 && min < max)
        {
            // A scale that bottoms out in mute: show its quietest real step.
            if (min == SND_CTL_TLV_DB_GAIN_MUTE &&
                snd_mixer_selem_get_playback_volume_range(_elem, &rawMin, &rawMax) >= 0 &&
                rawMin < rawMax)
                snd_mixer_selem_ask_playback_vol_dB(_elem, rawMin + 1, &min);
            log_i("volume: '%s' on %s (%.1f..%.1f dB%s), %d%%", _control.c_str(), _device.c_str(),
                  min / 100.0, max / 100.0, _hasSwitch ? ", switch" : "", level);
        }
        else
            log_i("volume: '%s' on %s (no dB scale%s), %d%%", _control.c_str(), _device.c_str(),
                  _hasSwitch ? ", switch" : "", level);
    }
    else
        log_i("volume: software gain, %d%%", level);
}

void AudioVolume::stop()
{
    int save = -1;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_started)
            return;
        _started = false;
        // Mute does not carry across runs: give the card its level back.
        if (_muted.load(std::memory_order_relaxed) && _elem != nullptr)
        {
            _muted.store(false, std::memory_order_relaxed);
            apply();
        }
        if (_unsaved)
        {
            _unsaved = false;
            save = _level.load(std::memory_order_relaxed);
        }
        closeMixer();
    }
    if (save >= 0)
        Settings::setUser("volume", std::to_string(save));
}

// The current level and mute to the control, or to the software gain.
void AudioVolume::apply()
{
    if (_mixer != nullptr && (snd_mixer_handle_events(_mixer) < 0 || !findControl()))
    {
        // Events take in changes made by others (the mixer skips writing a
        // value it believes is set already). An error, or the control gone:
        // the card went away -- try it afresh.
        closeMixer();
        openMixer();
    }
    const int level = _level.load(std::memory_order_relaxed);
    const bool silent = _muted.load(std::memory_order_relaxed) || level <= 0;
    if (_elem == nullptr)
    {
        const double v = level / 100.0;
        _softGainQ15.store(silent ? 0 : (int32_t)std::lround(32768.0 * v * v * v),
                           std::memory_order_relaxed);
        return;
    }
    bool ok = true;
    int32_t soft = 32768;
    if (_hasSwitch)
        ok = snd_mixer_selem_set_playback_switch_all(_elem, silent ? 0 : 1) >= 0;
    if (silent && !_hasSwitch)
    {
        if (_muteStep)
        {
            ok = setLevel(_elem, 0) && ok; // the bottom of its scale is silent
            _atMuteStep = true;
        }
        else
            soft = 0; // the control cannot go silent: AudioMixer's gain does it
    }
    else if (!silent && (_ownLevel || _atMuteStep))
    {
        ok = setLevel(_elem, level) && ok;
        _atMuteStep = false;
    }
    _softGainQ15.store(soft, std::memory_order_relaxed);
    if (!ok)
        log_w("volume: cannot set '%s'", _control.c_str());
}

// A user change: shown at once, saved a moment later (tick()).
void AudioVolume::changed()
{
    _unsaved = true;
    _changedAt = mono_ms();
    _generation.fetch_add(1, std::memory_order_acq_rel);
}

void AudioVolume::set(int level)
{
    level = std::clamp(level, 0, 100);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_started || (level == _level.load(std::memory_order_relaxed) && !_muted.load()))
        return;
    _level.store(level, std::memory_order_relaxed);
    _muted.store(false, std::memory_order_relaxed);
    _ownLevel = true;
    apply();
    changed();
}

void AudioVolume::step(int direction)
{
    const int size = std::max(1, (int)Settings::volumeStep);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_started)
        return;
    const int now = _level.load(std::memory_order_relaxed);
    const int level = std::clamp(now + (direction > 0 ? size : -size), 0, 100);
    if (level == now && !_muted.load())
        return;
    _level.store(level, std::memory_order_relaxed);
    _muted.store(false, std::memory_order_relaxed); // any volume key unmutes
    _ownLevel = true;
    apply();
    changed();
}

void AudioVolume::toggleMute()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_started)
        return;
    _muted.store(!_muted.load(), std::memory_order_relaxed);
    apply();
    // Mute itself is not saved: a head unit that starts silent looks broken.
    // A level taken over from the card is, before its first mute: should the
    // app die muted on a control muted by its bottom step, the next start
    // must not take that step over as the level.
    if (_muted.load() && Settings::volume.value < 0 && !_unsaved)
    {
        _unsaved = true;
        _changedAt = mono_ms();
    }
    _generation.fetch_add(1, std::memory_order_acq_rel);
}

void AudioVolume::tick()
{
    if (_reapply.exchange(false, std::memory_order_acq_rel))
    {
        // The output was (re)opened: a card that came back gets the level,
        // one that was missing gets another try.
        std::lock_guard<std::mutex> lock(_mutex);
        if (_started)
        {
            if (_mixer == nullptr && strcasecmp(_wanted.c_str(), "software") != 0)
                openMixer();
            apply();
        }
    }
    int save;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_unsaved || !due(mono_ms(), _changedAt + SAVE_AFTER_MS))
            return;
        _unsaved = false;
        save = _level.load(std::memory_order_relaxed);
    }
    Settings::setUser("volume", std::to_string(save));
}
