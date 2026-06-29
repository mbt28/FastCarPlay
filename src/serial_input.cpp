#include "serial_input.h"

#ifdef USE_CEDAR

#include <poll.h>
#include <unistd.h>
#include <cstdio>

#include "protocol/connection.h"
#include "protocol/protocol_const.h"
#include "protocol/message.h"
#include "common/logger.h"

#define BTN_UP 113 // present in the keyMap but not #defined in protocol_const.h

SerialInput::SerialInput(Connection &conn)
    : _conn(conn), _active(false)
{
    // Only run when stdin is a real interactive terminal. If FastCarPlay is
    // backgrounded (e.g. `... </dev/null &` to watch top/free), stdin is not a
    // tty -> no reader thread, no raw-mode change, no busy loop, no SIGTTIN.
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &_orig) != 0)
        return;

    // Non-canonical, no-echo so single keypresses arrive immediately. ISIG is
    // left enabled so Ctrl-C still quits.
    struct termios raw = _orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        _raw = true;

    _active = true;
    fprintf(stderr, "[Serial] test nav: arrows=move  enter/space=select  bksp=back  "
                    "h=home  n/p=track  i=siri  (or a/d/w/s)\n");
    _thread = std::thread(&SerialInput::loop, this);
}

SerialInput::~SerialInput()
{
    _active = false;
    if (_thread.joinable())
        _thread.join();
    if (_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &_orig);
}

void SerialInput::loop()
{
    while (_active)
    {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) // 200ms so we can re-check _active on exit
            continue;

        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 0)
            break; // EOF (terminal closed)
        if (n != 1)
            continue;

        int btn = 0;
        if (c == 0x1b) // ESC: arrow keys are ESC '[' 'A'/'B'/'C'/'D'
        {
            unsigned char s1, s2;
            if (read(STDIN_FILENO, &s1, 1) != 1 || s1 != '[')
                continue;
            if (read(STDIN_FILENO, &s2, 1) != 1)
                continue;
            switch (s2)
            {
            case 'A': btn = BTN_UP;    break;
            case 'B': btn = BTN_DOWN;  break;
            case 'C': btn = BTN_RIGHT; break;
            case 'D': btn = BTN_LEFT;  break;
            }
        }
        else
        {
            switch (c)
            {
            case '\r': case '\n': case ' ': // select = a down+up tap
                _conn.send(Message::Control(BTN_SELECT_DOWN));
                _conn.send(Message::Control(BTN_SELECT_UP));
                continue;
            case 0x7f: case 0x08: btn = BTN_BACK;           break; // del / backspace
            case 'h':             btn = BTN_HOME;           break;
            case 'n':             btn = BTN_NEXT_TRACK;     break;
            case 'p':             btn = BTN_PREVIOUS_TRACK; break;
            case 'i':             btn = BTN_SIRI;           break;
            case 'a':             btn = BTN_LEFT;           break;
            case 'd':             btn = BTN_RIGHT;          break;
            case 'w':             btn = BTN_UP;             break;
            case 's':             btn = BTN_DOWN;           break;
            }
        }

        if (btn)
        {
            log_d("[Serial] btn %d", btn);
            _conn.send(Message::Control(btn));
        }
    }
}

#endif /* USE_CEDAR */
