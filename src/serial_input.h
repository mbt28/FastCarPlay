#ifndef SRC_SERIAL_INPUT
#define SRC_SERIAL_INPUT

// TEST-ONLY input for the F1C200s build (USE_CEDRUS): the board has no SDL key
// input (dummy video driver) and touch is unplugged, so navigation is driven
// from the serial console (stdin). This is purely additive — it feeds the same
// protocol.send(Message::Control(btn)) path the normal input uses, and compiles
// to nothing on non-F1C builds.

#ifdef USE_CEDRUS

#include <atomic>
#include <thread>
#include <termios.h>

class IConnection;

class SerialInput
{
public:
    explicit SerialInput(IConnection &conn);
    ~SerialInput();

private:
    void loop();

    IConnection &_conn;
    std::atomic<bool> _active;
    std::thread _thread;
    struct termios _orig;
    bool _raw = false;
};

#endif /* USE_CEDRUS */
#endif /* SRC_SERIAL_INPUT */
