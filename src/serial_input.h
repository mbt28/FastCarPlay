#ifndef SRC_SERIAL_INPUT
#define SRC_SERIAL_INPUT

// TEST-ONLY input for the F1C200s build (USE_CEDAR): the board has no SDL key
// input (dummy video driver) and touch is unplugged, so navigation is driven
// from the serial console (stdin). This is purely additive — it feeds the same
// protocol.send(Message::Control(btn)) path the normal input uses, and compiles
// to nothing on non-Cedar builds.

#ifdef USE_CEDAR

#include <atomic>
#include <thread>
#include <termios.h>

class Connection;

class SerialInput
{
public:
    explicit SerialInput(Connection &conn);
    ~SerialInput();

private:
    void loop();

    Connection &_conn;
    std::atomic<bool> _active;
    std::thread _thread;
    struct termios _orig;
    bool _raw = false;
};

#endif /* USE_CEDAR */
#endif /* SRC_SERIAL_INPUT */
