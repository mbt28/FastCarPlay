#ifndef SRC_APPLICATION
#define SRC_APPLICATION

#include <SDL2/SDL.h>
#include <memory>

#include "protocol/protocol_const.h"

#include "protocol/connection.h"
#include "protocol/aa/aa_connection.h"
#ifdef USE_AA_WIRELESS
#include "protocol/aa/aa_wireless.h"
#endif
#ifdef USE_CP_WIRELESS
#include "protocol/cp/cp_connection.h"
#endif
#ifdef USE_CP_WIRED
#include "protocol/cp/cp_wired_connection.h"
#endif
#include "pipe_listener.h"
#include "renderer.h"

#define TOAST_TIME 3

class Application
{
public:
    Application(/* args */);
    ~Application();

    void start(const char *title);

private:
    struct State
    {
        bool dirty = false;
        bool frameRendered = false;
        int requestFrame = 0;
        bool fullscreen = false;
        bool mouseDown = false;
        int8_t latestState = PROTOCOL_STATUS_UNKNOWN;
        uint32_t showToast = false;
        std::string toast = "";
    };

    bool setAudioDriver();
    int processKey(SDL_Keysym key);
    bool processSystemEvent(const SDL_Event &e);
    bool processFrameEvents(AtomicQueue<Message> &queue, Renderer &renderer);
    const std::string status() const;

    void loop();
    void loopHeadless(); // no-renderer path: decoder presents to fb itself
    void loopDrm();      // DRM path: decoder presents video, UI on the overlay plane
#ifdef USE_LVGL
    void loopLvglTest(); // UI bring-up harness: LVGL screens only (lvgl-test)
    // Routes an SDL event to the UI (touch/mouse, and the 3-way encoder as
    // wheel or arrow keys). Returns true when the UI consumed it.
    bool feedUiEvent(class LvglOsd &osd, const SDL_Event &e);
#endif
    std::unique_ptr<class IDecoder> makeDecoder();
    std::unique_ptr<class IConnection> makeConnection();

    SDL_Window *_window;
    SDL_Renderer *_renderer;
    PipeListener *_keyListener;
    bool _active;
    SDL_DisplayMode _displayMode;
    State _state;
    int _width;
    int _height;
    bool _debug;
};

#endif /* SRC_APPLICATION */
