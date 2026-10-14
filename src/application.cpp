#include "application.h"

#include <cstdio>
#include <sstream>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <csignal>

#include "struct/video_buffer.h"
#include "common/clock.h"
#include "common/logger.h"

#include "settings.h"
#include "decoder.h"
#include "video_path.h" // probes how video is decoded + shown on this board

#include "ui/osd.h" // the built-in UI toolkit (defines USE_OSD_UI when one is built)
#ifdef USE_OSD_UI
#include "ui/ui_bridge.h" // native vars/actions the screens are wired to
#endif
#ifdef USE_CEDRUS
#include "v4l2drm_decoder.h" // mainline cedrus (ffmpeg v4l2-request) HW decoder (F1C200s)
#include "drm_display.h"     // shared DRM session: video plane + UI overlay plane
#include "serial_input.h"    // TEST-only serial-console navigation (F1C200s)
#endif
#ifdef __linux__
#include "touch_input.h" // evdev touchscreen; used on the drm/headless render paths
#include "key_input.h"   // Bluetooth keyboards/remotes (evdev, hotplugged)
#endif
#ifdef USE_BLUETOOTH
#include "bluetooth/bt_manager.h" // the one pairing agent + HID device management
#endif
#include "audio_mixer.h"
#include "common/functions.h"


Application::Application(/* args */) : _active(true)
{
    log_v("Creating");
}

Application::~Application()
{
    log_v("Destroying");
    log_d("Finished");
}

void Application::start(const char *)
{
    log_d("Initialising");

    if (video_path::detect().mode == video_path::Mode::Drm)
    {
        log_v("Starting (drm + UI overlay)");
        loopDrm();
    }
    else
    {
        log_v("Starting (no renderer)");
        loopHeadless();
    }
    log_v("Stopped");
}

// The decoder has to be started before the phone has told us anything, so it
// opens with the codec the backend expects. The real codec only arrives with
// the stream config, and a phone may pick H.264 even when HEVC is offered -- so
// rebuild the decoder when the backend reports a different one rather than
// decoding with the wrong codec. It is rebuilt, not just restarted, because the
// right backend can differ per codec (a chip may decode H.264 in hardware but
// not HEVC). Safe here: this runs on the render thread, and every buffer access
// goes through the pointer, so nothing holds a reference to the old decoder.
void Application::syncDecoderCodec(std::unique_ptr<IDecoder> &decoder, IConnection &protocol,
                                   AVCodecID &started)
{
    const AVCodecID want = protocol.videoCodec();

    // A hardware backend that gave up (could not open, or cannot decode this
    // stream) is retired for the rest of the session: rebuild on software so a
    // mismatch costs a rebuild, not a black screen.
    if (decoder->failed() && !_hwDisabled)
    {
        log_w("Hardware decode failed -- switching to software for this session");
        _hwDisabled = true;
        decoder->stop();
        decoder = makeDecoder(want);
        startDecoder(*decoder, protocol, want);
        started = want;
        return;
    }

    if (want == started)
        return;
    log_i("Video codec is now %s -- reopening the decoder", avcodec_get_name(want));
    decoder->stop();
    decoder = makeDecoder(want);
    startDecoder(*decoder, protocol, want);
    started = want;
}

#ifdef USE_OSD_UI
// Short, user-facing connection state for the picker header.
static const char *uiStatusText(int state)
{
    // Wired CarPlay has no dongle -- while it waits for a phone, prompt to plug in.
    if (Settings::carplayWired() &&
        (state == PROTOCOL_STATUS_NO_DEVICE || state == PROTOCOL_STATUS_INITIALISING ||
         state == PROTOCOL_STATUS_LINKING))
        return "Plug in iPhone";
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
#endif /* USE_OSD_UI */


namespace
{
// The loops have no event pump turning SIGINT into a quit event, so
// install our own handler for a clean Ctrl-C.
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

// The backend Settings::protocol selects in this build: an unknown string or
// a backend compiled out falls back to the first one the build carries.
// makeConnection() builds it, and the Bluetooth policy asks it too, so both
// agree on whether a phone pairs over Bluetooth.
enum class Backend : uint8_t
{
    CpWireless,
    CpWired,
    AaWireless,
    AaUsb,
};

Backend resolveBackend()
{
#ifdef USE_AA_WIRELESS
    if (Settings::aaWireless())
        return Backend::AaWireless;
#endif
#ifdef USE_CP_WIRELESS
    if (Settings::carplayWireless())
        return Backend::CpWireless;
#endif
#ifdef USE_CP_WIRED
    if (Settings::carplayWired())
        return Backend::CpWired;
#endif
    if (Settings::aaUsb())
        return Backend::AaUsb;
#if defined(USE_CP_WIRELESS)
    return Backend::CpWireless;
#elif defined(USE_CP_WIRED)
    return Backend::CpWired;
#elif defined(USE_AA_WIRELESS)
    return Backend::AaWireless;
#else
    return Backend::AaUsb;
#endif
}

#ifdef USE_BLUETOOTH
bool wirelessBackend()
{
    const Backend b = resolveBackend();
    return b == Backend::CpWireless || b == Backend::AaWireless;
}

// The Bluetooth manager brackets the connection: up before it (its agent must
// already answer when a phone pairs) and down after it. Declare it before the
// connection so destruction runs in that order.
struct BluetoothScope
{
    BluetoothScope() { bt::Manager::instance().start(wirelessBackend()); }
    ~BluetoothScope() { bt::Manager::instance().stop(); }
};
#endif

// Discovery competes with a wireless phone session for the shared radio, so
// the manager hears when one starts and ends. ONLINE counts: the phone is
// already on the access point, setting the session up. (The Bluetooth
// handoff before it the manager sees itself: the phone is connected.)
void noteSession(int state)
{
#ifdef USE_BLUETOOTH
    bt::Manager::instance().setSessionActive(
        wirelessBackend() && (state == PROTOCOL_STATUS_ONLINE || state == PROTOCOL_STATUS_CONNECTED));
#else
    (void)state;
#endif
}
} // namespace

std::unique_ptr<IDecoder> Application::makeDecoder(AVCodecID codecId)
{
    // The cedrus HW decoder is only linked in on USE_CEDRUS builds; otherwise
    // (and by default) the software avcodec Decoder is used. Hardware decode
    // is only worth taking when we own a DRM plane to hand the dma-buf to
    // (see video_path.h), and only for a codec this chip has a block for --
    // otherwise fall through to software, which always works.
#ifdef USE_CEDRUS
    if (!_hwDisabled && video_path::hwAvailable(codecId))
        return std::make_unique<V4l2DrmDecoder>();
#endif
    (void)codecId;
    return std::make_unique<Decoder>();
}

bool Application::decoderPresentable(AVCodecID codecId) const
{
    const video_path::Mode mode = video_path::detect().mode;
#ifdef USE_CEDRUS
    // On the DRM path only the hardware decoder presents (to the video
    // plane); loopDrm shows no software frames. Headless shows nothing.
    if (mode == video_path::Mode::Drm)
        return !_hwDisabled && video_path::hwAvailable(codecId);
#endif
    return false;
}

void Application::startDecoder(IDecoder &decoder, IConnection &protocol, AVCodecID codecId)
{
    if (decoderPresentable(codecId))
    {
        decoder.start(&protocol.videoStream, codecId, protocol.keyframeOnDrop());
        return;
    }
    // Decoding frames nobody can show would pin the core for nothing; the
    // session itself stays useful (audio + control), only video is off. The
    // stream queue is bounded, so the unconsumed frames just get dropped.
    log_w("No presentable decoder for %s on this video path -- video disabled",
          avcodec_get_name(codecId));
    protocol.videoStream.close(); // no consumer: the stream reader must not wait on it
}

void Application::publishStatus(IConnection &protocol)
{
    // The two cheap fields gate the expensive ones: status() builds a string on
    // every call in some backends, and this runs once per rendered frame.
    const int state = protocol.state();
    const bool focused = protocol.videoFocused();
    if (!_statusFile.due(state, focused))
        return;

    StatusFile::Fields f;
    f.state = state;
    f.videoFocused = focused;
    f.status = protocol.status();
    f.phoneName = protocol.phoneName();
    f.protocol = Settings::protocol.value;
    f.transfered = protocol.transfered();
    _statusFile.update(f);
}

DisplayGeometry Application::resolveGeometry() const
{
    DisplayGeometry g;

#ifdef USE_CEDRUS
    if (video_path::detect().mode == video_path::Mode::Drm && drm_display::width() > 0)
    {
        g.width = drm_display::width();
        g.height = drm_display::height();
        g.widthMm = drm_display::widthMm();
        g.heightMm = drm_display::heightMm();
        g.source = "DRM panel";
        return g;
    }
#endif

    // Headless, or a DRM display that never came up. Nothing to ask.
    g.width = Settings::width;
    g.height = Settings::height;
    g.source = "settings (no display)";
    return g;
}

std::unique_ptr<IConnection> Application::makeConnection()
{
    // Unknown protocol string or a backend compiled out: resolveBackend()
    // falls back to the first native backend this build carries rather than
    // failing to start; say why.
#ifndef USE_AA_WIRELESS
    if (Settings::aaWireless())
        log_e("protocol = aa-wireless needs a USE_AA_WIRELESS build");
#endif
#ifndef USE_CP_WIRELESS
    if (Settings::carplayWireless())
        log_e("protocol = carplay-wireless needs a USE_CP_WIRELESS build");
#endif
#ifndef USE_CP_WIRED
    if (Settings::carplayWired())
        log_e("protocol = carplay-wired needs a USE_CP_WIRED build");
#endif
    if (!Settings::aaWireless() && !Settings::carplayWireless() && !Settings::carplayWired() &&
        !Settings::aaUsb())
        log_e("unknown protocol '%s' -- using default", Settings::protocol.value.c_str());

    std::unique_ptr<IConnection> connection;
    switch (resolveBackend())
    {
#ifdef USE_AA_WIRELESS
    case Backend::AaWireless:
        connection = std::make_unique<AaWirelessConnection>();
        break;
#endif
#ifdef USE_CP_WIRELESS
    case Backend::CpWireless:
        connection = std::make_unique<CpConnection>();
        break;
#endif
#ifdef USE_CP_WIRED
    case Backend::CpWired:
        connection = std::make_unique<CpWiredConnection>();
        break;
#endif
    default: // AaUsb: every build carries it
        connection = std::make_unique<AaConnection>();
        break;
    }

    // Every loop has its display up by the time it builds the connection, so
    // this is where the phone's stream size is decided. Logged because a wrong
    // size is otherwise invisible -- it just looks like a soft picture.
    if (!_geometry.valid())
        _geometry = resolveGeometry();
    log_i("Display %dx%d (%s)%s", _geometry.width, _geometry.height, _geometry.source,
          _geometry.widthMm > 0 ? "" : " -- no physical size reported");
    connection->setDisplay(_geometry);
    return connection;
}

// No-UI path: nothing draws on the overlay plane. The decoder presents
// frames itself (cedrus -> DRM plane) and navigation comes from the serial
// console. Keeps the process alive and drives the protocol state.
void Application::loopHeadless()
{
#ifdef USE_BLUETOOTH
    BluetoothScope bluetooth;
#endif
    std::unique_ptr<IConnection> protocolPtr = makeConnection();
    IConnection &protocol = *protocolPtr;
    std::unique_ptr<IDecoder> decoder = makeDecoder(protocol.videoCodec());
    AudioMixer audioMixer;

    startDecoder(*decoder, protocol, protocol.videoCodec());
    AVCodecID startedCodec = protocol.videoCodec(); // reopened if the phone picks another
    audioMixer.start(&protocol.audioStreamMain, &protocol.audioStreamAux);
    protocol.start();

#ifdef __linux__
    TouchInput touchInput(protocol);   // evdev touchscreen (board loops only)
    KeyInput keyInput(protocol);       // Bluetooth keyboards/remotes -> the phone
#endif
#ifdef USE_CEDRUS
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
        syncDecoderCodec(decoder, protocol, startedCodec);
        auto state = protocol.state();
        if (state != lastState)
        {
            if (state == PROTOCOL_STATUS_CONNECTED)
            {
                decoder->flush();
                decoder->buffer.reset();
                protocol.send(Message::Control(BTN_SCREEN_REFRESH));
            }
            noteSession(state);
            lastState = state;
        }
        // Drain the buffer so a buffering (software) decoder can't stall; the
        // HW decoder presents to the DRM plane directly and leaves this empty.
        decoder->buffer.consume(&frame, &frameId);
        publishStatus(protocol);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// DRM path: the decoder presents video frames itself on the DRM/DEFE video
// plane; the UI goes on the overlay plane above it, writing straight into the
// plane's dumb framebuffer (no intermediate texture or composition surface).
// Input comes from the touchscreen/serial listeners as in the headless path.
void Application::loopDrm()
{
#ifdef USE_CEDRUS
    if (!drm_display::open("UI"))
    {
        log_w("DRM display unavailable; falling back to headless");
        loopHeadless();
        return;
    }

    if (!drm_display::uiBegin())
    {
        // No usable overlay plane: video still works, UI doesn't.
        log_w("DRM UI overlay unavailable; running headless");
        drm_display::close();
        loopHeadless();
        return;
    }

    // The panel is up now, so its size is known before anything that needs it.
    _geometry = resolveGeometry();

    const int uiWidth = drm_display::width();
    const int uiHeight = drm_display::height();

#ifdef USE_OSD_UI
    // The built-in screens own the overlay plane while idle, flushing
    // straight into its dumb framebuffer in the plane's own format.
    Osd osd;
    if (Settings::ui && !osd.beginDrm(uiWidth, uiHeight))
        log_w("UI unavailable > running without screens (video still works)");
#endif

#ifdef USE_BLUETOOTH
    BluetoothScope bluetooth;
#endif
    std::unique_ptr<IConnection> protocolPtr = makeConnection();
    IConnection &protocol = *protocolPtr;
    std::unique_ptr<IDecoder> decoder = makeDecoder(protocol.videoCodec());
    AudioMixer audioMixer;

    startDecoder(*decoder, protocol, protocol.videoCodec());
    AVCodecID startedCodec = protocol.videoCodec(); // reopened if the phone picks another
    audioMixer.start(&protocol.audioStreamMain, &protocol.audioStreamAux);
    protocol.start();

#ifdef __linux__
    TouchInput touchInput(protocol);   // evdev touchscreen (board loops only)
#ifdef USE_OSD_UI
    // Feed the primary finger to the UI (normalized -> panel pixels) while a
    // screen is up; routeToUi() below flips per frame based on what is shown.
    if (osd.active())
        touchInput.setUiSink([&osd, uiWidth, uiHeight](float nx, float ny, bool pressed) {
            osd.pointer((int)(nx * uiWidth), (int)(ny * uiHeight), pressed);
        });
#endif
    // Bluetooth keyboards/remotes: the phone, or the UI while a screen is up
    // (routed exactly like touch).
    KeyInput keyInput(protocol);
#ifdef USE_OSD_UI
    if (osd.active())
        keyInput.setUiSink([&osd](OsdKey key, char ch, int value) { osd.key(key, ch, value); });
#endif
#endif
    SerialInput serialInput(protocol); // TEST-only serial-console navigation

    g_quit = 0;
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);

    auto lastState = PROTOCOL_STATUS_UNKNOWN;
    // The cedrus v4l2 pool (~19MB of the 24MB CMA) exists only while a phone is
    // connected: it is torn down on disconnect and rebuilt on reconnect, so an
    // idle head unit holds no decode CMA and a reconnect allocates from a
    // near-empty pool. Starts true (decoder was start()ed just above); the first
    // non-CONNECTED state tears it down.
    bool decoderRunning = true;
    bool uiShowsHome = true;
    bool osdShown = false;
    uint32_t framesAtConnect = drm_display::videoFrames();
    AVFrame *frame = nullptr;
    uint32_t frameId = 0;

    while (_active && !g_quit)
    {
        // Only sync/rebuild-on-codec-change while the decoder is live; a torn-down
        // (disconnected) decoder has nothing to sync and must not be restarted
        // here -- the state block below owns its lifecycle.
        if (decoderRunning)
            syncDecoderCodec(decoder, protocol, startedCodec);
        publishStatus(protocol);
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
                // Rebuild from scratch on (re)connect: the previous disconnect
                // tore the decoder down to free its cedrus CMA pool, so setup()
                // here allocates fresh from a near-empty CMA region. A plain
                // flush() would keep the old pool pinned and the phone's fresh
                // SPS would then OOM ffmpeg's v4l2 CREATE_BUFS ("create buffers
                // failed for type 2") with only ~1MB CMA free -- the reconnect
                // hang that needed a reboot. (If the decoder is somehow still
                // live -- e.g. a direct UNKNOWN->CONNECTED first connect -- a
                // flush suffices.)
                if (!decoderRunning)
                {
                    decoder = makeDecoder(startedCodec);
                    startDecoder(*decoder, protocol, startedCodec);
                    decoderRunning = true;
                }
                else
                    decoder->flush();
                decoder->buffer.reset();
                framesAtConnect = drm_display::videoFrames();
                protocol.send(Message::Control(BTN_SCREEN_REFRESH));
            }
            else if (decoderRunning)
            {
                // Phone went away: tear the decoder down now so its ~19MB cedrus
                // pool is returned to CMA immediately instead of sitting pinned
                // (which is what makes the next allocation fail). stop() joins
                // the decode thread, so teardown()'s releaseHeld + flushVideo
                // run before we touch the decoder again.
                decoder->stop();
                decoderRunning = false;
            }
            noteSession(state);
            lastState = state;
            dirty = true;
        }

        if (!videoActive)
        {
#ifdef USE_OSD_UI
            if (osd.active())
            {
                // The UI owns the home screen on the overlay plane: source
                // picker + settings, driven by the touchscreen.
                const bool backgrounded =
                    state == PROTOCOL_STATUS_CONNECTED && !protocol.videoFocused();
                ui_bridge::setBackgroundedSession(backgrounded);
                ui_bridge::setStatus(backgrounded ? "Session paused" : uiStatusText(state));
                if (ui_bridge::takeResumeRequest())
                    protocol.requestVideoFocus();
#ifdef __linux__
                touchInput.routeToUi(true);
                keyInput.routeToUi(true);
#endif
                // UI flushes land straight in the dumb framebuffer, so
                // presenting is just making sure the plane covers the panel.
                // An unchanged screen costs one timer tick and nothing else.
                bool uiChanged = osd.tick();
                if (dirty || !uiShowsHome)
                {
                    // Full repaint: first show, return from video, or an
                    // app-side state change (the buffer may hold OSD rows).
                    osd.invalidate();
                    uiChanged = true;
                }
                if (uiChanged)
                    drm_display::uiShow(0, uiHeight);
                uiShowsHome = true;
                osdShown = false;

                if (ui_bridge::restartRequested())
                    _active = false; // main re-execs into the chosen source
            }
            else
#endif
            {
                // No UI available (disabled or failed to start): the overlay stays
                // hidden; video and the session still run.
                if (uiShowsHome || osdShown)
                {
                    drm_display::uiHide();
                    osdShown = false;
                }
                uiShowsHome = true;
            }
        }
        else
        {
#if defined(USE_OSD_UI) && defined(__linux__)
            touchInput.routeToUi(false); // video is up: touch goes to the phone
            keyInput.routeToUi(false);
#endif
            // Video plays below; the overlay hides entirely.
            if (uiShowsHome || osdShown)
            {
                drm_display::uiHide();
                osdShown = false;
#ifdef USE_OSD_UI
                if (osd.active())
                    osd.hidden();
#endif
            }
            uiShowsHome = false;
        }

        // Drain the buffer so a buffering (software) decoder can't stall; the
        // HW decoders present directly and leave this empty.
        decoder->buffer.consume(&frame, &frameId);
        // While our own UI is on screen, tick faster so the toolkit's input
        // and refresh timers run every period and touch feedback lands
        // within one frame; this is cheap now that an unchanged screen costs
        // nothing. With video up the loop only shepherds the session.
        std::this_thread::sleep_for(std::chrono::milliseconds(videoActive ? 50 : 16));
    }

    drm_display::uiHide();
    drm_display::close();
#else
    loopHeadless();
#endif /* USE_CEDRUS */
}
