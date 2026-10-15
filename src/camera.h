#ifndef SRC_CAMERA
#define SRC_CAMERA

// A V4L2 capture device -- e.g. a composite reversing camera on the F1C200s
// TV decoder -- shown full screen on the DRM video plane with no copy: its
// buffers are exported as dma-bufs and scanned out as they are, so it must
// give NV12 in contiguous memory. Settings::camera* say which device, input,
// standard and picture levels. Used by Settings > Video > Test camera.

#if defined(USE_CEDRUS) && defined(__linux__)

#include <atomic>
#include <string>
#include <thread>
#include <vector>

class Camera
{
public:
    // What Settings > Video shows. Levels are 0-100 % of the control's
    // range, -1 where the device has no such control.
    struct Info
    {
        bool present = false;
        std::vector<std::string> inputs;
        int brightness = -1, contrast = -1, saturation = -1;
    };
    // Opens the device for a moment: call when the screen is entered, not
    // every tick.
    static Info probe();

    ~Camera() { stop(); }

    // Opens the device, applies the settings and streams onto the video
    // plane, keeping the decoder's frames off it. False with error() set.
    bool start();
    void stop();
    bool running() const { return _fd >= 0; }
    // The stream broke after a good start; error() says how.
    bool failed() const { return _failed.load(); }
    const std::string &error() const { return _error; }

private:
    void loop();
    bool queue(int index);
    void fail(const std::string &why);

    int _fd = -1;
    std::vector<int> _dmabufs; // one exported dma-buf per capture buffer
    int _width = 0, _height = 0, _pitch = 0;
    bool _streaming = false; // on the video plane, decoder frames held off
    std::thread _thread;
    std::atomic<bool> _run{false};
    std::atomic<bool> _failed{false};
    std::string _error;
};

#endif
#endif /* SRC_CAMERA */
