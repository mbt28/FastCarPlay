#ifndef SRC_CAMERA
#define SRC_CAMERA

// A V4L2 capture device -- e.g. a composite reversing camera on the F1C200s
// TV decoder -- shown full screen on the DRM video plane with no copy: its
// buffers are exported as dma-bufs and scanned out as they are, so it must
// give NV12 in contiguous memory. Settings > Video > Camera opens it with a
// settings column over the picture (the input, its standard and its picture
// levels); every change applies at once, and each input keeps its own
// settings (camera-0 .. camera-3).
//
// Everything runs on the main loop thread except the capture thread, which
// only dequeues frames, shows them and queues them back.

#if defined(USE_CEDRUS) && defined(__linux__)

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

class Camera
{
public:
    static Camera &instance();

    // Is there a camera at all (opens the device for a moment).
    static bool present();

    // A picture level (or a menu control) as the column shows it.
    struct Level
    {
        std::string label;
        std::string value;
    };

    ~Camera() { stop(); }

    // Opens the device with the last input's settings and streams onto the
    // video plane, keeping the decoder's frames off it. False with error().
    bool start();
    // Saves what is not saved yet and lets go of the device and the plane.
    void stop();
    bool running() const { return _fd >= 0; }
    // The stream broke after a good start; error() says how.
    bool failed() const { return _failed.load(); }
    const std::string &error() const { return _error; }
    // The last start or restart failed for lack of video memory.
    bool outOfMemory() const { return _noMemory; }

    // Every loop turn while running: a due restart, the once-a-second check
    // of the signal (and, on Auto, of the standard), the delayed save.
    void tick();

    // What the column shows; any change bumps generation().
    const std::vector<std::string> &inputs() const { return _inputs; }
    int input() const { return _input; }
    bool noSignal() const { return _noSignal; }
    bool hasStandard() const { return _hasStd; }
    const char *standardLabel() const;
    const std::vector<Level> &levels() const { return _levels; }
    uint32_t generation() const { return _generation; }

    // Live: the levels follow at once; the stream restarts only for a
    // standard it is not in.
    void selectInput(int input);
    // Auto -> PAL -> NTSC (direction +1) and back.
    void cycleStandard(int direction);
    // Level `level` one step up (+1) or down (-1).
    void adjustLevel(int level, int direction);

private:
    struct Control
    {
        uint32_t id;
        const char *key;   // in the saved profile
        const char *label; // in the column
        int min, max, step, def, value;
        bool menu;
        std::vector<std::pair<int, std::string>> items; // a menu's entries
    };
    struct Profile
    {
        std::string standard = "auto";
        std::vector<std::pair<std::string, int>> values; // raw, by Control::key
        int get(const char *key, int fallback) const;
        void set(const char *key, int value);
    };

    Camera() = default;
    void scanControls();
    void configure(v4l2_std_id std);
    void applyProfile();
    bool streamOn();
    void streamOff(bool release);
    void restart();
    bool queue(int index);
    void fail(const std::string &why);
    void loop();
    void buildLevels();
    void changed(bool save);
    void save();
    Profile &profile() { return _profiles[_input < 4 ? _input : 0]; }
    const Profile &profile() const { return _profiles[_input < 4 ? _input : 0]; }

    // Device
    int _fd = -1;
    std::vector<int> _dmabufs; // one exported dma-buf per capture buffer
    int _width = 0, _height = 0, _pitch = 0;
    bool _streaming = false; // on the video plane, decoder frames held off
    std::thread _thread;
    std::atomic<bool> _run{false};
    std::atomic<bool> _failed{false};
    std::string _error;
    bool _noMemory = false;

    // Settings
    std::vector<std::string> _inputs;
    std::vector<Control> _controls;
    Profile _profiles[4];
    int _input = 0;
    bool _hasStd = false;    // the current input takes a video standard
    v4l2_std_id _std = 0;    // the one set
    bool _noSignal = false;
    uint32_t _restartAt = 0; // 0: none due
    v4l2_std_id _restartStd = 0;
    uint32_t _pollAt = 0;
    bool _unsaved = false;
    uint32_t _saveAt = 0;
    bool _held = false;      // holdVideo(true) is ours to undo

    std::vector<Level> _levels;
    uint32_t _generation = 0;
};

#endif
#endif /* SRC_CAMERA */
