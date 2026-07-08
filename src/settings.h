#ifndef SRC_SETTINGS
#define SRC_SETTINGS

#include "common/settings_base.h"

#define SCREEN_MODE_WINDOW 0
#define SCREEN_MODE_FULLSCREEN 1
#define SCREEN_MODE_HEADLESS 2

// The singleton “Settings” namespace
class Settings
{
public:
    // General section
    // Protocol backend. "carlinkit" (default) = the Carlinkit dongle protocol.
    // "aa-usb" = native wired Android Auto: the phone is plugged straight into
    // the USB host port, switched to accessory mode (AOAP) and spoken to
    // directly -- no dongle.
    static inline Setting<std::string> protocol{"protocol", "carlinkit"};
    static inline bool aaUsb() { return protocol.value == "aa-usb"; }
    static inline Setting<int> vendorid{"vendor-id", 4884};
    // 0 = auto-detect the Carlinkit dongle (scan for vendor-id + a known
    // product id, 0x1520/0x1521). Set a specific product id to pin one device.
    static inline Setting<int> productid{"product-id", 0};
    // Native AA device selection: 0 = scan all devices for AOAP support;
    // set both to pin a specific phone.
    static inline Setting<int> aaVendorid{"aa-vendor-id", 0};
    static inline Setting<int> aaProductid{"aa-product-id", 0};
    // Native AA video stream: 1=800x480, 2=1280x720, 3=1920x1080 / 30 or 60 fps.
    static inline Setting<int> aaResolution{"aa-resolution", 1};
    static inline Setting<int> aaFps{"aa-video-fps", 30};
    // Wireless Android Auto (protocol = aa-wireless): the head unit runs a
    // Wi-Fi AP and advertises the AA profile over Bluetooth; the phone joins
    // the AP and connects to the TCP server. Needs hostapd + dnsmasq + BlueZ.
    static inline Setting<std::string> wifiIface{"wifi-interface", "wlan0"};
    static inline Setting<std::string> wifiSsid{"wifi-ssid", "FastCarPlay"};
    static inline Setting<std::string> wifiPass{"wifi-passphrase", "carplay1234"};
    static inline Setting<int> wifiChannel{"wifi-channel", 6}; // 2.4GHz (ESP32)
    static inline Setting<std::string> apIp{"wifi-ap-ip", "192.168.53.1"};
    static inline Setting<std::string> btName{"bluetooth-name", "FastCarPlay"};
    static inline bool aaWireless() { return protocol.value == "aa-wireless"; }
    // ms to wait for the phone to re-enumerate in accessory mode after the
    // AOAP switch (first connections show a consent dialog on the phone).
    static inline Setting<int> aaAccessoryTimeout{"aa-accessory-timeout", 5000};
    // Media ack window offered in the AV setup response.
    static inline Setting<int> aaMaxUnacked{"aa-max-unacked", 4};
    static inline Setting<int> width{"width", 1024};
    static inline Setting<int> height{"height", 576};
    static inline Setting<int> sourceFps{"source-fps", 60};
    static inline Setting<int> screenMode{"window-mode", 0};
    static inline Setting<bool> cursor{"cursor", false};
    static inline Setting<int> loglevel{"log-level", 2};

    // Device configurations section
    static inline Setting<bool> encryption{"encryption", false};
    static inline Setting<bool> autoconnect{"autoconnect", true};
    static inline Setting<bool> weakCharge{"weak-charge", true};
    static inline Setting<bool> leftDrive{"left-hand-drive", true};
    static inline Setting<int> nightMode{"night-mode", 2};
    static inline Setting<bool> wifi5{"wifi-5", true};
    static inline Setting<bool> bluetoothAudio{"bluetooth-audio", false};
    static inline Setting<int> micType{"mic-type", 1};
    static inline Setting<int> dpi{"android-dpi", 120};
    static inline Setting<int> androidMode{"android-resolution", 1};
    static inline Setting<int> mediaDelay{"android-media-delay", 300};

    // Application configuration section
    static inline Setting<int> fontSize{"font-size", 40};
    static inline Setting<bool> vsync{"vsync", false};
    static inline Setting<bool> hwDecode{"hw-decode", true};
    // Use the Allwinner Cedar hardware decoder (libcedarc) instead of ffmpeg.
    // Only honoured on builds compiled with USE_CEDAR (e.g. the F1C200s).
    static inline Setting<bool> cedar{"cedar-decode", false};
    // Use the mainline cedrus decoder via ffmpeg's V4L2-Request hwaccel (blob-free,
    // no libcedarc). Decodes to a tiled-NV12 dma-buf presented on the DEFE.
    // Only honoured on builds compiled with USE_CEDRUS (e.g. the F1C200s).
    static inline Setting<bool> cedrus{"cedrus-decode", false};
    // Display backend. "sdl" = the SDL renderer (default). "drm" = the decoder
    // presents video on the DRM/DEFE plane and the UI (home screen, toasts,
    // debug) is drawn on an ARGB overlay plane above it -- same interface as
    // the SDL path, no SDL video driver needed. "none" = no UI at all
    // (lightest): a minimal loop, the decoder presents frames itself.
    static inline Setting<std::string> renderer{"renderer", "sdl"};
    // Anything other than "sdl" (none, drm, ...) skips the SDL video subsystem
    // and window. The decoder presents frames itself.
    static inline bool noRenderer() { return renderer.value != "sdl"; }
    // The DRM path additionally drives the UI overlay plane (needs TTF).
    static inline bool drmUi() { return renderer.value == "drm"; }
    // Touchscreen for the headless DRM/DEFE path (read directly from evdev).
    // Empty device = auto-detect the first node with an absolute-position axis
    // (e.g. the GT911). swap/invert are for panel orientation calibration.
    static inline Setting<std::string> touchDevice{"touch-device", ""};
    static inline Setting<bool> touchSwapXY{"touch-swap-xy", false};
    static inline Setting<bool> touchInvertX{"touch-invert-x", false};
    static inline Setting<bool> touchInvertY{"touch-invert-y", false};
    static inline Setting<int> renderingBuffer{"rendering-buffer", 5};
    static inline Setting<int> eventsSkip{"draw-skip-events", 3};
    static inline Setting<int> forceRedraw{"force-redraw", 0};
    static inline Setting<float> aspectCorrection{"aspect-correction", 1};
    static inline Setting<std::string> renderDriver{"renderer-driver", ""};
    static inline Setting<bool> alternativeRendering{"alternative-rendering", false};
    static inline Setting<bool> fastScale{"fast-render-scale", false};
    static inline Setting<int> usbQueue{"async-usb-calls", 32};
    static inline Setting<int> usbTransferSize{"usb-buffer-size", 2048};  
    static inline Setting<int> usbBuffer{"usb-buffer", 128};    
    static inline Setting<int> audioDelay{"audio-buffer-wait", 2};
    static inline Setting<int> audioDelayCall{"audio-buffer-wait-call", 6};
    static inline Setting<float> audioFade{"audio-fade", 0.3};
    static inline Setting<int> audioAuxDelay{"audio-aux-delay", 200};
    static inline Setting<int> audioBuffer{"audio-buffer-samples", 512};
    static inline Setting<std::string> audioDriver{"audio-driver", ""};
    static inline Setting<std::string> onConnect{"on-connect-script", ""};
    static inline Setting<std::string> onDisconnect{"on-disconnect-script", ""};

    // Key mapping section
    static inline Setting<std::string> keyPipe{"key-pipe-path", ""};
    static inline KeySetting<int> keySiri{"key-siri", 115, 5};
    static inline KeySetting<int> keyNightOn{"key-nightmode-on", 122, 16};
    static inline KeySetting<int> keyNightOff{"key-nightmode-off", 120, 17};
    static inline KeySetting<int> keyLeft{"key-left", 1073741904, 100};
    static inline KeySetting<int> keyLeftExtra{"key-left-extra", 39, 100};
    static inline KeySetting<int> keyRight{"key-right", 1073741903, 101};
    static inline KeySetting<int> keyRightExtra{"key-right-extra", 92, 101};
    static inline KeySetting<int> keyEnter{"key-enter", 13, 104};
    static inline KeySetting<int> keyEnterUp{"key-enterup", 0, 105};
    static inline KeySetting<int> keyBack{"key-back", 8, 106};
    static inline KeySetting<int> keyUp{"key-up", 1073741906, 113};
    static inline KeySetting<int> keyDown{"key-down", 1073741905, 114};
    static inline KeySetting<int> keyHome{"key-home", 104, 200};
    static inline KeySetting<int> keyPlay{"key-play", 93, 201};
    static inline KeySetting<int> keyPause{"key-pause", 91, 202};
    static inline KeySetting<int> keyPlayPause{"key-play-toggle", 112, 203};
    static inline KeySetting<int> keyNext{"key-next", 46, 204};
    static inline KeySetting<int> keyPrev{"key-previous", 44, 205};
    static inline KeySetting<int> keyAccept{"key-call-accept", 97, 300};
    static inline KeySetting<int> keyReject{"key-call-reject", 115, 301};
    static inline KeySetting<int> keyVideoFocus{"key-video-focus", 118, 500};
    static inline KeySetting<int> keyVideoRelease{"key-video-release", 98, 501};
    static inline KeySetting<int> keyNavFocus{"key-nav-focus", 110, 508};
    static inline KeySetting<int> keyNavRelease{"key-nav-release", 109, 509};

    // Custom scripts
    static inline Setting<std::string> script1{"custom-script-1", ""};
    static inline Setting<int> scriptKey1{"key-custom-script-1", 49};
    static inline Setting<std::string> scriptName1{"custom-script-name-1", ""};
    static inline Setting<std::string> script2{"custom-script-2", ""};
    static inline Setting<int> scriptKey2{"key-custom-script-2", 50};
    static inline Setting<std::string> scriptName2{"custom-script-name-2", ""};
    static inline Setting<std::string> script3{"custom-script-3", ""};
    static inline Setting<int> scriptKey3{"key-custom-script-3", 51};
    static inline Setting<std::string> scriptName3{"custom-script-name-3", ""};
    static inline Setting<std::string> script4{"custom-script-4", ""};
    static inline Setting<int> scriptKey4{"key-custom-script-4", 52};
    static inline Setting<std::string> scriptName4{"custom-script-name-4", ""};

    // Debug section
    static inline Setting<bool> codecLowDelay{"decode-low-delay", true};
    static inline Setting<bool> codecFast{"decode-fast", true};
    static inline Setting<bool> debugOverlay{"debug-overlay", false};

    static bool load(const std::string &filename);
    static void print();

    static inline bool isFullscreen() { return screenMode == SCREEN_MODE_FULLSCREEN; };
    static inline bool isHeadless() { return screenMode == SCREEN_MODE_HEADLESS; };

private:
    static void trim(std::string &s);
};

#endif /* SRC_SETTINGS */
