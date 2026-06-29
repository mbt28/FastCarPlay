#ifndef SRC_TOUCH_INPUT
#define SRC_TOUCH_INPUT

// Headless touch input for the F1C200s DRM/DEFE build (USE_CEDAR). The board has
// no SDL video path (frames go straight to the DEFE), so SDL_FINGER events are
// unavailable. Instead we read the touchscreen (e.g. GT911) directly from its
// Linux evdev device and feed normalized touch coordinates into the same
// CarPlay protocol the SDL path uses (Message::Click / Message::Move).
// Additive + USE_CEDAR-gated; compiles to nothing on other builds.

#ifdef USE_CEDAR

#include <atomic>
#include <thread>

class Connection;

class TouchInput
{
public:
    explicit TouchInput(Connection &conn);
    ~TouchInput();

private:
    bool openDevice();
    void loop();

    Connection &_conn;
    std::atomic<bool> _active;
    std::thread _thread;
    int _fd = -1;
    int _xmin = 0, _xmax = 1, _ymin = 0, _ymax = 1;
};

#endif /* USE_CEDAR */
#endif /* SRC_TOUCH_INPUT */
