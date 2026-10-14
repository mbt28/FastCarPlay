#ifndef SRC_AUDIO_VOLUME
#define SRC_AUDIO_VOLUME

// The app's output volume: 0-100 % plus mute, kept across restarts in the
// user settings. Applied through the sound card's own playback control when
// it has one -- ALSA's simple mixer, so any Linux card ("Master", "PCM",
// "Headphone"...) -- mapped through the control's dB scale so that equal
// steps sound about equal; otherwise (or when configured so) as a software
// gain in AudioMixer.
//
// Callable from any thread: keys arrive on the key reader's, the screens on
// the loop's.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;

class AudioVolume
{
public:
    static AudioVolume &instance();

    // Finds the control (see the volume-control setting) and applies the
    // saved level, or takes over the card's current one on first use.
    void start();
    // Saves a level not saved yet, undoes a mute on the card, closes the
    // mixer.
    void stop();

    int level() const { return _level.load(std::memory_order_relaxed); } // 0..100
    bool muted() const { return _muted.load(std::memory_order_relaxed); }

    void set(int level);      // 0..100; unmutes
    void step(int direction); // +1 / -1 by the volume-step setting; unmutes
    void toggleMute();

    // The gain AudioMixer applies: Q15, 32768 = unity, 0 = silent. Unity
    // while a mixer control does the work (0 when that control cannot go
    // silent by itself).
    int32_t softwareGainQ15() const { return _softGainQ15.load(std::memory_order_relaxed); }

    // AudioMixer opened the PCM -- possibly on a card that was unplugged
    // and is back at its power-on level. The next tick() re-applies.
    void outputOpened() { _reapply.store(true, std::memory_order_release); }

    // Bumped on every change; the screens re-read level() and muted().
    uint32_t generation() const { return _generation.load(std::memory_order_acquire); }

    // Call from a loop: re-applies after outputOpened(), and saves the level
    // once it has been left alone for a moment, so holding a volume key does
    // not rewrite the settings file at every step.
    void tick();

private:
    AudioVolume() = default;
    ~AudioVolume();
    AudioVolume(const AudioVolume &) = delete;
    AudioVolume &operator=(const AudioVolume &) = delete;

    // All below run with _mutex held.
    bool openMixer();
    void closeMixer();
    bool findControl(); // (again) after events: the element may be new
    void apply();       // the current level/mute -> control or gain
    void changed();

    std::mutex _mutex; // the mixer handle and everything below
    std::string _device;  // ALSA control device
    std::string _wanted;  // the volume-control setting
    std::string _control; // the chosen control's name (empty: software)
    snd_mixer_t *_mixer = nullptr;
    snd_mixer_elem_t *_elem = nullptr; // null: software gain
    bool _hasSwitch = false;
    bool _muteStep = false;   // the control's lowest step is silent
    bool _ownLevel = false;   // the level is ours to write (not taken over)
    bool _atMuteStep = false; // we silenced it by writing that step
    bool _quiet = false;      // a mixer failure is logged once, until it works
    bool _started = false;
    bool _unsaved = false;
    uint32_t _changedAt = 0;

    std::atomic<int> _level{100};
    std::atomic<bool> _muted{false};
    std::atomic<bool> _reapply{false};
    std::atomic<int32_t> _softGainQ15{32768};
    std::atomic<uint32_t> _generation{0};
};

#endif /* SRC_AUDIO_VOLUME */
