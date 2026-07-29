#include "application.h"

#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <sstream>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <csignal>

#include "struct/video_buffer.h"
#include "common/logger.h"

#include "settings.h"
#include "interface.h"
#include "decoder.h"

#ifdef USE_LVGL
#include "ui/lvgl_osd.h"  // LVGL bound to SDL_Renderer; EEZ Studio screens
#include "ui/ui_bridge.h" // native vars/actions the screens are wired to
#endif
#ifdef USE_CEDRUS
#include "cedrus_decoder.h" // mainline cedrus (ffmpeg v4l2-request) HW decoder (F1C200s)
#endif
#if defined(USE_CEDAR) || defined(USE_CEDRUS)
#include "drm_display.h" // shared DRM session: video plane + UI overlay plane
#include "interface.h"
#endif
#ifdef __linux__
#include "touch_input.h" // evdev touchscreen; used on the drm/headless render paths
#endif
#ifdef USE_CEDAR
#include "cedar_decoder.h" // Allwinner Cedar HW H.264 decoder (F1C200s)
#include "serial_input.h"  // TEST-only serial-console navigation (F1C200s)
#endif
#include "pcm_audio.h"
#include "common/functions.h"

static KeySetting<int> *keyMap[] = {
    &Settings::keySiri,
    &Settings::keyNightOn,
    &Settings::keyNightOff,
    &Settings::keyLeft,
    &Settings::keyLeftExtra,
    &Settings::keyRight,
    &Settings::keyRightExtra,
    &Settings::keyEnter,
    &Settings::keyEnterUp,
    &Settings::keyBack,
    &Settings::keyUp,
    &Settings::keyDown,
    &Settings::keyHome,
    &Settings::keyPlay,
    &Settings::keyPause,
    &Settings::keyPlayPause,
    &Settings::keyNext,
    &Settings::keyPrev,
    &Settings::keyAccept,
    &Settings::keyReject,
    &Settings::keyVideoFocus,
    &Settings::keyVideoRelease,
    &Settings::keyNavFocus,
    &Settings::keyNavRelease};

static constexpr size_t keyMapSize = sizeof(keyMap) / sizeof(keyMap[0]);

Application::Application(/* args */) : _window(nullptr),
                                       _renderer(nullptr),
                                       _keyListener(nullptr),
                                       _active(true)
{
    log_v("Creating");

    _debug = Settings::debugOverlay;

    if (!setAudioDriver())
        throw std::runtime_error("Unsupported audio driver " + std::string(Settings::audioDriver.value));

    // Without a renderer we need no video subsystem, fonts or display mode.
    Uint32 sdlSubsystems = SDL_INIT_TIMER | SDL_INIT_AUDIO;
    if (!Settings::noRenderer())
        sdlSubsystems |= SDL_INIT_VIDEO;
    if (SDL_Init(sdlSubsystems) != 0)
        throw std::runtime_error(std::string("SDL initialisation failed > ") + SDL_GetError());

    if (!Settings::noRenderer() || Settings::drmUi())
    {
        // The DRM-UI path renders text too; TTF needs no SDL video driver.
        if (TTF_Init() != 0)
        {
            SDL_Quit();
            throw std::runtime_error(std::string("TTF initialisation failed > ") + TTF_GetError());
        }
    }

    if (!Settings::noRenderer())
    {
        if (SDL_GetCurrentDisplayMode(0, &_displayMode) != 0)
        {
            TTF_Quit();
            SDL_Quit();
            throw std::runtime_error(std::string("SDL get display mode failed > ") + SDL_GetError());
        }

        log_i("SDL screen %dx%d@%d, audio driver %s", _displayMode.w, _displayMode.h, _displayMode.refresh_rate, SDL_GetCurrentAudioDriver());
    }
    else
    {
        log_i("No renderer; audio driver %s", SDL_GetCurrentAudioDriver());
    }
}

Application::~Application()
{
    log_v("Destroying");
    if (_keyListener != nullptr)
    {
        delete _keyListener;
        _keyListener = nullptr;
    }
    if (_renderer != nullptr)
        SDL_DestroyRenderer(_renderer);
    if (_window != nullptr)
        SDL_DestroyWindow(_window);
    TTF_Quit();
    SDL_Quit();
    log_d("Finished");
}

void Application::start(const char *title)
{
    log_d("Initialising");

    if (Settings::drmUi())
    {
        log_v("Starting (drm + UI overlay)");
        loopDrm();
        log_v("Stopped");
        return;
    }

    if (Settings::noRenderer())
    {
        log_v("Starting (no renderer)");
        loopHeadless();
        log_v("Stopped");
        return;
    }

    // Create SDL window centered on screen
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, Settings::fastScale ? "nearest" : "best");

    // Prepare window, show it in headless to avoid blinking, otherwise hidden untill iniailised
    bool fullsize = Settings::isFullscreen() || Settings::isHeadless();
    _width = fullsize ? _displayMode.w : Settings::width;
    _height = fullsize ? _displayMode.h : Settings::height;
    _window = SDL_CreateWindow(title,
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               _width,
                               _height,
                               SDL_WINDOW_RESIZABLE | (Settings::isHeadless() ? 0 : SDL_WINDOW_HIDDEN));

    if (!_window)
        throw std::runtime_error(std::string("SDL can't create window > ") + SDL_GetError());

    if (!Settings::cursor)
        SDL_ShowCursor(SDL_DISABLE);

    // Create renderer for the window
#ifdef USE_CEDAR
    // F1C200s has no GPU and SDL has no usable display backend; video is painted
    // directly to /dev/fb0 by CedarDecoder. SDL just needs a (software) renderer
    // for the UI; run it with SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy.
    Uint32 flags = SDL_RENDERER_SOFTWARE;
#else
    Uint32 flags = SDL_RENDERER_ACCELERATED;
#endif
    if (Settings::vsync)
        flags |= SDL_RENDERER_PRESENTVSYNC;

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, Settings::renderDriver.value.c_str());
    _renderer = SDL_CreateRenderer(_window, -1, flags);

    if (!_renderer)
        throw std::runtime_error(std::string("SDL can't create renderer > ") + SDL_GetError());

    SDL_RendererInfo rendererInfo{};
    if (SDL_GetRendererInfo(_renderer, &rendererInfo) == 0)
    {
        log_i("Renderer %s (%s, %s)", rendererInfo.name,
              ((rendererInfo.flags & SDL_RENDERER_ACCELERATED) ? "accelerated" : "software"),
              ((rendererInfo.flags & SDL_RENDERER_PRESENTVSYNC) ? "vsync" : "no-vsync"));
    }

#ifdef USE_LVGL
    if (Settings::lvglTest)
    {
        log_v("Starting (LVGL UI bring-up)");
        SDL_ShowWindow(_window);
        loopLvglTest();
        log_v("Stopped");
        return;
    }
#endif

    log_v("Starting");
    loop();
    log_v("Stopped");
}

#ifdef USE_LVGL
// Short, user-facing connection state for the picker header. Deliberately not
// Application::status(), which is a multi-line diagnostic dump for the debug
// overlay. Mirrors the wording of Interface::drawHome.
static const char *uiStatusText(int state)
{
    switch (state)
    {
    case PROTOCOL_STATUS_ERROR:
        return "Device error";
    case PROTOCOL_STATUS_NO_DEVICE:
        return "No device";
    case PROTOCOL_STATUS_INITIALISING:
    case PROTOCOL_STATUS_LINKING:
        return "Initialising";
    case PROTOCOL_STATUS_ONLINE:
        return "Connect phone";
    case PROTOCOL_STATUS_CONNECTED:
        return "Connecting";
    default:
        return "Select source";
    }
}

// Head units frequently have only a 3-way rotary encoder, so every screen has
// to be reachable without a touchscreen. On the desktop the encoder is
// emulated by the mouse wheel and the arrow keys, with Enter as the push.
bool Application::feedUiEvent(LvglOsd &osd, const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_MOUSEMOTION:
        osd.pointer(e.motion.x, e.motion.y, (e.motion.state & SDL_BUTTON_LMASK) != 0);
        return true;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        osd.pointer(e.button.x, e.button.y, e.type == SDL_MOUSEBUTTONDOWN);
        return true;

    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        osd.pointer((int)(e.tfinger.x * _width), (int)(e.tfinger.y * _height),
                    e.type != SDL_FINGERUP);
        return true;

    case SDL_MOUSEWHEEL:
        osd.encoder(-e.wheel.y, false); // wheel up = previous row
        return true;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        switch (e.key.keysym.sym)
        {
        case SDLK_LEFT:
        case SDLK_UP:
            if (e.type == SDL_KEYDOWN)
                osd.encoder(-1, false);
            return true;
        case SDLK_RIGHT:
        case SDLK_DOWN:
            if (e.type == SDL_KEYDOWN)
                osd.encoder(1, false);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            osd.encoder(0, e.type == SDL_KEYDOWN);
            return true;
        default:
            break;
        }
        return false; // let Escape/quit fall through to the normal handler
    }
    return false;
}

// UI bring-up harness (lvgl-test = true): renders the EEZ Studio screens on
// their own, with no connection, decoder or audio. Its only job is to prove
// the LVGL -> SDL_Renderer path, the input plumbing and the generated screens
// before any of it is wired into the real loops.
void Application::loopLvglTest()
{
    LvglOsd osd;
    if (!osd.begin(_renderer, _width, _height))
    {
        log_e("LVGL bring-up failed");
        return;
    }

    _active = true;
    uint32_t frames = 0;
    const uint32_t started = SDL_GetTicks();

    // Scripted clicks for the capture harness: "x,y" or "x,y;x,y;..." to walk
    // a flow (open Settings, toggle a row...) with no human involved.
    std::vector<SDL_Point> clicks;
    {
        const std::string &spec = Settings::lvglTestClick.value;
        for (std::size_t at = 0; at < spec.size();)
        {
            std::size_t end = spec.find(';', at);
            if (end == std::string::npos)
                end = spec.size();
            SDL_Point point{};
            if (sscanf(spec.c_str() + at, "%d,%d", &point.x, &point.y) == 2)
                clicks.push_back(point);
            at = end + 1;
        }
    }
    // Scripted encoder steps: numbers rotate, "p" pushes.
    std::vector<std::string> encoderSteps;
    {
        const std::string &spec = Settings::lvglTestEncoder.value;
        for (std::size_t at = 0; at < spec.size();)
        {
            std::size_t end = spec.find(',', at);
            if (end == std::string::npos)
                end = spec.size();
            std::string token = spec.substr(at, end - at);
            if (!token.empty())
                encoderSteps.push_back(token);
            at = end + 1;
        }
    }

    // One input every 12 frames, then time to settle before the capture.
    const uint32_t inputs = (uint32_t)(clicks.size() + encoderSteps.size());
    const uint32_t captureFrame = 30 + inputs * 12;

    while (_active)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                _active = false;
            else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                _active = false;
            else
                feedUiEvent(osd, event);
        }

        // A source change is persisted immediately but only takes effect on
        // restart; exiting hands that to the init script, which respawns us.
        if (ui_bridge::restartRequested())
        {
            log_i("Restarting to apply the new source");
            _active = false;
        }

        SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
        SDL_RenderClear(_renderer);
        osd.render();

        frames++;

        // Encoder script runs first, then any clicks.
        for (std::size_t i = 0; i < encoderSteps.size(); i++)
        {
            const uint32_t at = 8 + (uint32_t)i * 12;
            const std::string &token = encoderSteps[i];
            if (token == "p")
            {
                if (frames == at)
                    osd.encoder(0, true);
                else if (frames == at + 4)
                    osd.encoder(0, false);
            }
            else if (frames == at)
            {
                osd.encoder(atoi(token.c_str()), false);
            }
        }

        // Press, then release a few frames later so LVGL sees a real CLICKED.
        for (std::size_t i = 0; i < clicks.size(); i++)
        {
            const uint32_t press = 8 + (uint32_t)(encoderSteps.size() + i) * 12;
            if (frames == press)
                osd.pointer(clicks[i].x, clicks[i].y, true);
            else if (frames == press + 4)
                osd.pointer(clicks[i].x, clicks[i].y, false);
        }

        // Capture mode: let the UI settle, read the frame straight back from
        // the renderer (before Present, so the target is still valid) and quit.
        if (!Settings::lvglTestShot.value.empty() && frames == captureFrame)
        {
            SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, _width, _height, 32,
                                                               SDL_PIXELFORMAT_ARGB8888);
            if (shot != nullptr &&
                SDL_RenderReadPixels(_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                     shot->pixels, shot->pitch) == 0 &&
                SDL_SaveBMP(shot, Settings::lvglTestShot.value.c_str()) == 0)
                log_i("LVGL capture %dx%d > %s", _width, _height,
                      Settings::lvglTestShot.value.c_str());
            else
                log_e("LVGL capture failed > %s", SDL_GetError());

            if (shot != nullptr)
                SDL_FreeSurface(shot);
            _active = false;
        }

        SDL_RenderPresent(_renderer);
        SDL_Delay(16);
    }

    const uint32_t elapsed = SDL_GetTicks() - started;
    if (elapsed > 0)
        log_i("LVGL bring-up: %u frames in %u ms (%.1f fps)", frames, elapsed,
              frames * 1000.0f / elapsed);
    osd.end();
}
#endif

bool Application::setAudioDriver()
{
    if (Settings::audioDriver.value.length() < 2)
        return true;

    for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i)
    {
        if (SDL_GetAudioDriver(i) == Settings::audioDriver.value)
        {
            SDL_setenv("SDL_AUDIODRIVER", Settings::audioDriver.value.c_str(), 1);
            return true;
        }
    }
    return false;
}

int Application::processKey(SDL_Keysym key)
{
    for (uint8_t i = 0; i < keyMapSize; i++)
    {
        if (keyMap[i]->value == key.sym)
        {
            return keyMap[i]->key;
        }
    }
    log_w("Unmapped key %d", key.sym);
    return 0;
}

bool Application::processSystemEvent(const SDL_Event &e)
{
    if (e.type == SDL_QUIT)
    {
        _active = false;
        return true;
    }

    if (e.type == SDL_WINDOWEVENT)
    {
        if (e.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            SDL_GetWindowSize(_window, &_width, &_height);
            _state.dirty = true;
        }
        return true;
    }

    if (e.type == SDL_KEYDOWN)
    {
        switch (e.key.keysym.sym)
        {
        case SDLK_f:
        {
            if (Settings::isHeadless())
                return true;
            _state.fullscreen = !_state.fullscreen; // Toggle fullscreen mode
            SDL_SetWindowFullscreen(_window, _state.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            SDL_SetWindowBordered(_window, _state.fullscreen ? SDL_FALSE : SDL_TRUE);
            return true;
        }
        case SDLK_q:
        {
            _active = false;
            return true;
        }
#ifndef NDEBUG
        case SDLK_d:
        {
            _debug = !_debug;
            return true;
        }
#endif
        case SDLK_r:
        {
            _state.dirty = true;
            _state.requestFrame = 1;
        }
        }
        bool script = false;
        std::string name = "";
        std::string scriptPath = "";
        if (e.key.keysym.sym == Settings::scriptKey1)
        {
            script = true;
            name = Settings::scriptName1;
            scriptPath = Settings::script1;
        }
        if (e.key.keysym.sym == Settings::scriptKey2)
        {
            script = true;
            name = Settings::scriptName2;
            scriptPath = Settings::script2;
        }
        if (e.key.keysym.sym == Settings::scriptKey3)
        {
            script = true;
            name = Settings::scriptName3;
            scriptPath = Settings::script3;
        }
        if (e.key.keysym.sym == Settings::scriptKey4)
        {
            script = true;
            name = Settings::scriptName4;
            scriptPath = Settings::script4;
        }

        if (script)
        {
            if (name.length() > 0)
            {
                _state.toast = name;
                _state.showToast = 1;
            }

            if (scriptPath.length() > 1)
            {
                execute(scriptPath.c_str());
            }
        }
    }

    return false;
}

bool Application::processFrameEvents(AtomicQueue<Message> &queue, Renderer &renderer)
{
    bool result = false;
    SDL_Event e;
    bool motion = false;
    int motionX = 0;
    int motionY = 0;
    int downX = -1;
    int downY = -1;
    int upX = -1;
    int upY = -1;

    while (SDL_PollEvent(&e))
    {
        if (processSystemEvent(e))
            continue;

        switch (e.type)
        {

        case SDL_MOUSEBUTTONDOWN:
        {
            _state.mouseDown = true;
            downX = e.button.x;
            downY = e.button.y;
            break;
        }

        case SDL_MOUSEBUTTONUP:
        {
            _state.mouseDown = false;
            upX = e.button.x;
            upY = e.button.y;
            result = true;
            break;
        }
        case SDL_MOUSEMOTION:
        {
            if (!_state.mouseDown)
                break;
            motionX = e.motion.x;
            motionY = e.motion.y;
            motion = true;
            break;
        }
        case SDL_KEYDOWN:
        {
            int key = processKey(e.key.keysym);
            if (key > 0)
            {
                queue.pushDiscard(Message::Control(key));
                result = true;
            }
            break;
        }
        case SDL_KEYUP:
        {
            if (e.key.keysym.sym == Settings::keyEnter)
            {
                queue.pushDiscard(Message::Control(Settings::keyEnterUp.key));
                result = true;
            }
            break;
        }
        }
    }

    if (_state.frameRendered && (downX >= 0 || upX >= 0 || motion))
    {
        if (downX >= 0)
            queue.pushDiscard(Message::Click(renderer.xScale * downX / _width, renderer.yScale * downY / _height, true));
        if (motion)
            queue.pushDiscard(Message::Move(renderer.xScale * motionX / _width, renderer.yScale * motionY / _height));
        if (upX >= 0)
            queue.pushDiscard(Message::Click(renderer.xScale * upX / _width, renderer.yScale * upY / _height, false));
    }

    return result;
}

namespace
{
// The SDL loop relies on SDL turning SIGINT into an SDL_QUIT event; the headless
// loop has no SDL event pump, so install our own handler for a clean Ctrl-C.
volatile std::sig_atomic_t g_quit = 0;
// First signal asks for a clean shutdown; a second one takes it. Teardown
// touches USB, threads and the DRM master, so if any of that ever wedges the
// user must still be able to stop the app without resorting to SIGKILL.
void onQuitSignal(int)
{
    if (g_quit)
        _exit(1);
    g_quit = 1;
}
} // namespace

std::unique_ptr<IDecoder> Application::makeDecoder()
{
    // Cedar HW decoder is selectable at runtime but only linked in on USE_CEDAR
    // builds; otherwise (and by default) the software avcodec Decoder is used.
#ifdef USE_CEDRUS
    if (Settings::cedrus)
        return std::make_unique<CedrusDecoder>();
#endif
#ifdef USE_CEDAR
    if (Settings::cedar)
        return std::make_unique<CedarDecoder>();
#endif
    return std::make_unique<Decoder>();
}

std::unique_ptr<IConnection> Application::makeConnection()
{
#ifdef USE_AA_WIRELESS
    if (Settings::aaWireless())
        return std::make_unique<AaWirelessConnection>();
#else
    if (Settings::aaWireless())
        log_w("protocol = aa-wireless needs a USE_AA_WIRELESS build, using carlinkit");
#endif
#ifdef USE_CP_WIRELESS
    if (Settings::carplayWireless())
        return std::make_unique<CpConnection>();
#else
    if (Settings::carplayWireless())
        log_w("protocol = carplay-wireless needs a USE_CP_WIRELESS build, using carlinkit");
#endif
    if (Settings::aaUsb())
        return std::make_unique<AaConnection>();
    return std::make_unique<Connection>();
}

// No-renderer path: no SDL window/renderer/fonts are created. The decoder
// presents frames itself (Cedar -> /dev/fb0) and navigation comes from the
// serial console. Keeps the process alive and drives the protocol state.
void Application::loopHeadless()
{
    std::unique_ptr<IConnection> protocolPtr = makeConnection();
    IConnection &protocol = *protocolPtr;
    std::unique_ptr<IDecoder> decoder = makeDecoder();
    PcmAudio audioMain("main"), audioAux("aux");

    decoder->start(&protocol.videoStream, protocol.videoCodec());
    audioMain.start(&protocol.audioStreamMain);
    audioAux.start(&protocol.audioStreamAux, &audioMain);
    protocol.start();

#ifdef __linux__
    TouchInput touchInput(protocol);   // evdev touchscreen (renderer = drm/none)
#endif
#ifdef USE_CEDAR
    SerialInput serialInput(protocol); // TEST-only serial-console navigation
#endif

    // Clean exit on Ctrl-C / SIGTERM; the SerialInput dtor then restores the tty.
    g_quit = 0;
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);

    auto lastState = PROTOCOL_STATUS_UNKNOWN;
    AVFrame *frame = nullptr;
    uint32_t frameId = 0;
    while (_active && !g_quit)
    {
        auto state = protocol.state();
        if (state != lastState)
        {
            if (state == PROTOCOL_STATUS_CONNECTED)
            {
                decoder->flush();
                decoder->buffer.reset();
                protocol.send(Message::Control(BTN_SCREEN_REFRESH));
            }
            lastState = state;
        }
        // Drain the buffer so a buffering (software) decoder can't stall; the
        // Cedar decoder presents to fb directly and leaves this empty.
        decoder->buffer.consume(&frame, &frameId);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// DRM path: the decoder presents video frames itself on the DRM/DEFE video
// plane; the UI (home screen while not streaming, toasts, the debug overlay)
// is drawn with the regular Interface code through an SDL *software*
// renderer into the ARGB overlay plane above the video. No SDL video driver
// is used; input comes from the touchscreen/serial listeners as in the
// headless path.
void Application::loopDrm()
{
#if defined(USE_CEDAR) || defined(USE_CEDRUS)
    if (!drm_display::open("UI"))
    {
        log_w("DRM display unavailable; falling back to headless");
        loopHeadless();
        return;
    }

    SDL_Renderer *uiRenderer = drm_display::uiRenderer();
    if (!uiRenderer)
    {
        // No ARGB overlay plane: video still works, UI doesn't.
        log_w("DRM UI overlay unavailable; running headless");
        drm_display::close();
        loopHeadless();
        return;
    }

    Interface interface(uiRenderer);
    interface.drawHome(true, PROTOCOL_STATUS_UNKNOWN, "");
    drm_display::uiPresent();

    const int uiWidth = drm_display::width();
    const int uiHeight = drm_display::height();
#ifdef USE_LVGL
    // The LVGL screens replace the plain status home screen on the overlay
    // plane, driven by the touchscreen. Same renderer as Interface -- they are
    // used mutually exclusively (LVGL when idle, Interface's OSD over video).
    LvglOsd osd;
    if (Settings::lvglUi && !osd.begin(uiRenderer, uiWidth, uiHeight))
        log_w("LVGL UI unavailable > using the plain home screen");
#endif

    std::unique_ptr<IConnection> protocolPtr = makeConnection();
    IConnection &protocol = *protocolPtr;
    std::unique_ptr<IDecoder> decoder = makeDecoder();
    PcmAudio audioMain("main"), audioAux("aux");

    decoder->start(&protocol.videoStream, protocol.videoCodec());
    audioMain.start(&protocol.audioStreamMain);
    audioAux.start(&protocol.audioStreamAux, &audioMain);
    protocol.start();

#ifdef __linux__
    TouchInput touchInput(protocol);   // evdev touchscreen (renderer = drm/none)
#ifdef USE_LVGL
    // Feed the primary finger to the UI (normalized -> panel pixels) while a
    // screen is up; routeToUi() below flips per frame based on what is shown.
    if (osd.active())
        touchInput.setUiSink([&osd, uiWidth, uiHeight](float nx, float ny, bool pressed) {
            osd.pointer((int)(nx * uiWidth), (int)(ny * uiHeight), pressed);
        });
#endif
#endif
#ifdef USE_CEDAR
    SerialInput serialInput(protocol); // TEST-only serial-console navigation
#endif

    g_quit = 0;
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);

    auto lastState = PROTOCOL_STATUS_UNKNOWN;
    bool uiShowsHome = true;
    bool osdShown = false;
    uint32_t framesAtConnect = drm_display::videoFrames();
    Uint32 debugTick = 0;
    AVFrame *frame = nullptr;
    uint32_t frameId = 0;

    while (_active && !g_quit)
    {
        Uint32 now = SDL_GetTicks();
        auto state = protocol.state();
        uint32_t frames = drm_display::videoFrames();
        // "Video flowing" = connected and at least one frame was presented
        // in THIS session. No recency timeout: CarPlay streams are
        // event-driven, so a static screen legitimately sends no frames for
        // seconds - the home screen must not paint over the live video.
        // It returns only on disconnect (or if decode never started).
        bool videoActive = (state == PROTOCOL_STATUS_CONNECTED) &&
                           frames > framesAtConnect &&
                           protocol.videoFocused(); // exit hands the screen back

        bool dirty = false;
        if (state != lastState)
        {
            if (state == PROTOCOL_STATUS_CONNECTED)
            {
                decoder->flush();
                decoder->buffer.reset();
                framesAtConnect = drm_display::videoFrames();
                protocol.send(Message::Control(BTN_SCREEN_REFRESH));
            }
            lastState = state;
            dirty = true;
        }

        // Toast timing (parity with the SDL loop).
        if (_state.showToast > 0)
        {
            if (_state.showToast == 1)
            {
                interface.showToast(_state.toast);
                _state.showToast = now ? now : 1;
                dirty = true;
            }
            else if (now - _state.showToast >= TOAST_TIME * 1000)
            {
                interface.hideToast();
                _state.showToast = 0;
                dirty = true;
            }
        }

#ifndef NDEBUG
        if (_debug && now - debugTick >= 1000)
        {
            debugTick = now;
            char debugBuffer[512];
            std::snprintf(debugBuffer, sizeof(debugBuffer),
                          "DRM overlay %dx%d\n"
                          "FRAME: %u\n"
                          "USB: %s\n"
                          "BUFF: video [%u] audio[main %u aux %u] out [%u]",
                          drm_display::width(), drm_display::height(),
                          frames,
                          protocol.status().c_str(),
                          protocol.videoStream.count(),
                          protocol.audioStreamMain.count(),
                          protocol.audioStreamAux.count(),
                          protocol.writeQueue.count());
            interface.debug(debugBuffer);
            dirty = true;
        }
#endif

        if (!videoActive)
        {
#ifdef USE_LVGL
            if (osd.active())
            {
                // LVGL owns the home screen on the overlay plane: source
                // picker + settings, driven by the touchscreen.
                const bool backgrounded =
                    state == PROTOCOL_STATUS_CONNECTED && !protocol.videoFocused();
                ui_bridge::setBackgroundedSession(backgrounded);
                ui_bridge::setStatus(backgrounded ? "Session paused" : uiStatusText(state));
                if (ui_bridge::takeResumeRequest())
                    protocol.requestVideoFocus();
#ifdef __linux__
                touchInput.routeToUi(true);
#endif
                SDL_SetRenderDrawColor(uiRenderer, 0, 0, 0, 255);
                SDL_RenderClear(uiRenderer);
                osd.render();
                drm_display::uiPresent();
                uiShowsHome = true;
                osdShown = false;

                if (ui_bridge::restartRequested())
                    _active = false; // main re-execs into the chosen source
            }
            else
#endif
            {
                // Home screen (opaque) on the overlay; also covers stale video.
                if (interface.drawHome(dirty || !uiShowsHome, state, protocol.phoneName()))
                    drm_display::uiPresent();
                uiShowsHome = true;
                osdShown = false;
            }
        }
        else
        {
#if defined(USE_LVGL) && defined(__linux__)
            touchInput.routeToUi(false); // video is up: touch goes to the phone
#endif
            // Video plays below; overlay carries only toasts/debug, or hides.
            if (uiShowsHome || dirty)
            {
                if (interface.drawOsd())
                {
                    drm_display::uiPresent();
                    osdShown = true;
                }
                else if (uiShowsHome || osdShown)
                {
                    drm_display::uiHide();
                    osdShown = false;
                }
                uiShowsHome = false;
            }
        }

        // Drain the buffer so a buffering (software) decoder can't stall; the
        // HW decoders present directly and leave this empty.
        decoder->buffer.consume(&frame, &frameId);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    drm_display::uiHide();
    drm_display::close();
#else
    loopHeadless();
#endif
}

void Application::loop()
{
    // Prepare home screen
    Interface interface(_renderer);
    interface.drawHome(true, PROTOCOL_STATUS_UNKNOWN, "");

#ifdef USE_LVGL
    // lvgl-ui: the LVGL screens replace the plain status home screen, so a
    // source can be picked on the unit itself. Falls back to the old home
    // screen if it cannot start, rather than leaving a blank display.
    LvglOsd osd;
    uint32_t uiFrames = 0;
    if (Settings::lvglUi && !osd.begin(_renderer, _width, _height))
        log_w("LVGL UI unavailable > using the plain home screen");
#endif

    // Process full screen, do not do this in headless to avoid blinking
    if (Settings::isFullscreen())
    {
        _state.fullscreen = true;
        // FULLSCREEN_DESKTOP, not exclusive FULLSCREEN: the latter changes
        // the video mode and grabs the keyboard on X11, which locks the rest
        // of the desktop out. Matches what the "f" toggle already uses.
        SDL_SetWindowFullscreen(_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_SetWindowBordered(_window, SDL_FALSE);
    }

    // Show window, do not do this in headless to avoid blinking
    if (!Settings::isHeadless())
        SDL_ShowWindow(_window);
    interface.drawHome(true, PROTOCOL_STATUS_UNKNOWN, "");

    std::unique_ptr<IConnection> protocolPtr = makeConnection();
    IConnection &protocol = *protocolPtr;
    std::unique_ptr<IDecoder> decoder = makeDecoder();
    PcmAudio audioMain("main"), audioAux("aux");

    if (Settings::keyPipe.value.length() > 2)
        _keyListener = new PipeListener(Settings::keyPipe.value.c_str());

    decoder->start(&protocol.videoStream, protocol.videoCodec());
    audioMain.start(&protocol.audioStreamMain);
    audioAux.start(&protocol.audioStreamAux, &audioMain);
    protocol.start();

    // The SDL loop has an event pump, but a signal must still stop it: with a
    // window-less or unfocused window there is no other way out.
    g_quit = 0;
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);

    log_v("Loop");
    std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
    int32_t frameTime = 0;
    int32_t frameDelay = 0;
    const int32_t frameTarget = Settings::sourceFps > 0 ? 1000000 / Settings::sourceFps : 1000000;
    AVFrame *frame = nullptr;
    uint32_t frameId = 0;
    uint32_t dropframes = 0;
    int skipEvents = 0;
#ifndef NDEBUG
    Uint32 debugLast = SDL_GetTicks();
    int debugSpeed = 0;
    int debugLastCount = 0;
#endif
    while (_active && !g_quit)
    {
        bool newFrame = false;

        if (_state.showToast > 0)
        {
            if (_state.showToast == 1)
            {
                interface.showToast(_state.toast);
                _state.showToast = SDL_GetTicks();
                _state.dirty = true;
            }

            if (SDL_GetTicks() - _state.showToast >= TOAST_TIME * 1000)
            {
                interface.hideToast();
                _state.showToast = 0;
                _state.dirty = true;
            }
        }

        if (protocol.state() != _state.latestState)
        {
            // On connect/disconnect
            if (protocol.state() == PROTOCOL_STATUS_CONNECTED || _state.latestState == PROTOCOL_STATUS_CONNECTED)
            {
                _state.frameRendered = false;
                _state.dirty = true;
                _state.requestFrame = 0;
            }
            // On connect
            if (protocol.state() == PROTOCOL_STATUS_CONNECTED)
            {
                decoder->flush();
                decoder->buffer.reset();
            }
            _state.latestState = protocol.state();
        }

        // Connected but backgrounded: the phone handed the screen back after
        // the user pressed exit. The session stays up; we just show our own
        // UI instead of its video until the user goes back in.
        const bool projecting = protocol.videoFocused();
        if (!projecting)
            _state.frameRendered = false;

        if (_state.latestState == PROTOCOL_STATUS_CONNECTED && projecting)
        {
            uint32_t latestFrameId = 0;
            if (decoder->buffer.consume(&frame, &latestFrameId))
            {
                newFrame = latestFrameId != frameId;
                if (newFrame || _state.dirty)
                {
                    if (interface.render(frame))
                    {
                        _state.frameRendered = true;
                        _state.dirty = false;
                        if (frameId > 0 && latestFrameId - frameId > 1)
                        {
                            dropframes += latestFrameId - frameId - 1;
                            log_d("Frame drop %d on %d total %d", latestFrameId - frameId - 1, latestFrameId, dropframes);
                        }
                        frameId = latestFrameId;
                    }
                }
            }

            if (_state.requestFrame > 0 && Settings::forceRedraw > 0 && _state.requestFrame++ % Settings::forceRedraw == 0)
            {
                log_d("Request screen update");
                protocol.send(Message::Control(BTN_SCREEN_REFRESH));
                if (_state.requestFrame > Settings::forceRedraw * 2)
                    _state.requestFrame = 0;
            }
        }

        if (!_state.frameRendered)
        {
#ifdef USE_LVGL
            if (osd.active())
            {
                // LVGL owns the home screen: source picker + settings, driven
                // by touch and by a 3-way encoder.
                ui_bridge::setBackgroundedSession(
                    _state.latestState == PROTOCOL_STATUS_CONNECTED && !projecting);
                ui_bridge::setStatus(_state.latestState == PROTOCOL_STATUS_CONNECTED && !projecting
                                         ? "Session paused"
                                         : uiStatusText(_state.latestState));
                if (ui_bridge::takeResumeRequest())
                    protocol.requestVideoFocus();
                SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
                SDL_RenderClear(_renderer);
                osd.render();
                SDL_RenderPresent(_renderer);
                _state.dirty = false;

                SDL_Event e;
                while (SDL_PollEvent(&e))
                    if (!feedUiEvent(osd, e))
                        processSystemEvent(e);

                // Scripted click, so the pick -> persist -> restart path can be
                // exercised without a human (see tools/ui-sweep.sh).
                if (!Settings::lvglTestClick.value.empty() && ++uiFrames == 40)
                {
                    int cx = 0, cy = 0;
                    if (sscanf(Settings::lvglTestClick.value.c_str(), "%d,%d", &cx, &cy) == 2)
                        osd.pointer(cx, cy, true);
                }
                else if (!Settings::lvglTestClick.value.empty() && uiFrames == 44)
                {
                    int cx = 0, cy = 0;
                    if (sscanf(Settings::lvglTestClick.value.c_str(), "%d,%d", &cx, &cy) == 2)
                        osd.pointer(cx, cy, false);
                }

                // A source change is persisted; restarting is what actually
                // switches protocol, since the connection is built at start-up.
                if (ui_bridge::restartRequested())
                    _active = false;
            }
            else
#endif
            {
                interface.drawHome(_state.dirty, _state.latestState, protocol.phoneName());
                _state.dirty = false;
                SDL_Event e;
                while (SDL_PollEvent(&e))
                    processSystemEvent(e);
            }
        }
        else
        {
            if (newFrame || ++skipEvents > Settings::eventsSkip)
            {
                if (processFrameEvents(protocol.writeQueue, interface) && Settings::forceRedraw > 0)
                {
                    _state.requestFrame = 1;
                }
                skipEvents = 0;
            }
        }

#ifndef NDEBUG
        if (_debug)
        {
            if (SDL_GetTicks() - debugLast >= 1000)
            {
                debugSpeed = (protocol.transfered() - debugLastCount) / (SDL_GetTicks() - debugLast);
                debugLastCount = protocol.transfered();
                debugLast = SDL_GetTicks();
            }
            char debugBuffer[2048];
            std::snprintf(debugBuffer, sizeof(debugBuffer),
                          "%s\n"
                          "FRAME: %u / %u [%d] dropped: %d render: %dus / %dus\n"
                          "USB: %s ~%dKB/s\n"
                          "BUFF: video [%u] audio[main %u aux %u] out [%u]",
                          status().c_str(),
                          frameId,
                          decoder->buffer.latestId(),
                          decoder->buffer.latestId() - frameId,
                          dropframes,
                          frameTime,
                          frameDelay,
                          protocol.status().c_str(),
                          debugSpeed,
                          protocol.videoStream.count(),
                          protocol.audioStreamMain.count(),
                          protocol.audioStreamAux.count(),
                          protocol.writeQueue.count());
            interface.debug(debugBuffer);
        }
#endif

        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        frameTime = (int32_t)std::chrono::duration_cast<std::chrono::microseconds>(now - frameStart).count();
        frameStart = now;
        if (_active && !Settings::vsync && !_state.dirty)
        {
            frameDelay = (frameTarget - frameTime) * ((decoder->buffer.latestId() == frameId) ? 1.0 : 0.9);
            if (frameDelay > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(frameDelay));
                frameStart += std::chrono::microseconds(frameDelay);
            }
            else
            {
                // Overran the frame budget: without this the loop spins with
                // no sleep at all, pinning a core. On a head unit that only
                // wastes power, but on a desktop the busy SDL client makes the
                // whole session's input sticky. A millisecond is nothing
                // against a 33ms budget and guarantees the thread yields.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    if (!Settings::isHeadless())
        SDL_HideWindow(_window);
}

const std::string Application::status() const
{
    std::ostringstream out;

    SDL_version compiled{};
    SDL_VERSION(&compiled);
    SDL_version linked{};
    SDL_GetVersion(&linked);

    out << "SDL: v"
        << static_cast<int>(compiled.major) << '.'
        << static_cast<int>(compiled.minor) << '.'
        << static_cast<int>(compiled.patch) << " "
        << SDL_GetCurrentVideoDriver();

    SDL_Window *window = SDL_GetKeyboardFocus();
    SDL_RendererInfo cr{};
    if (window)
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        out << " " << width << 'x' << height;
        SDL_Renderer *renderer = SDL_GetRenderer(window);
        if (renderer)
        {
            if (SDL_GetRendererInfo(renderer, &cr) == 0)
            {
                out << ((cr.flags & SDL_RENDERER_ACCELERATED) != 0 ? " accelerated" : "")
                    << ((cr.flags & SDL_RENDERER_PRESENTVSYNC) != 0 ? " vsync" : "");
            }
        }
    }
    out << " audio: " << SDL_GetCurrentAudioDriver();

    out << "\nBACKENDS:";
    for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i)
    {
        SDL_RendererInfo info;
        SDL_GetRenderDriverInfo(i, &info);
        out << " ";
        if (cr.name == info.name)
            out << "[" << info.name << "]";
        else
            out << info.name;
    }

    int displayIndex = SDL_GetWindowDisplayIndex(window);
    if (displayIndex >= 0)
    {
        out << "\nSCREEN:";
        SDL_Rect bounds{};
        SDL_DisplayMode mode{};
        SDL_GetDisplayBounds(displayIndex, &bounds);
        if (SDL_GetCurrentDisplayMode(displayIndex, &mode) == 0)
        {
            out << " [" << displayIndex << "] "
                << bounds.w << 'x' << bounds.h
                << '@' << mode.refresh_rate
                << " " << SDL_GetPixelFormatName(mode.format);
        }
    }

    return out.str();
}
