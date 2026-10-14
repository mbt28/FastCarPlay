#ifndef SRC_SETTINGS
#define SRC_SETTINGS

#include "common/settings_base.h"

// The singleton “Settings” namespace
class Settings
{
public:
    // General section
    // Protocol backend: "carplay-wireless" (default), "carplay-wired",
    // "aa-wireless", or "aa-usb" (native wired Android Auto: the phone is
    // switched to accessory mode via AOAP and spoken to directly).
    static inline Setting<std::string> protocol{"protocol", "carplay-wireless"};
    static inline bool aaUsb() { return protocol.value == "aa-usb"; }
    // Native AA device selection: 0 = scan all devices for AOAP support;
    // set both to pin a specific phone.
    static inline Setting<int> aaVendorid{"aa-vendor-id", 0};
    static inline Setting<int> aaProductid{"aa-product-id", 0};
    // Android Auto video stream: 1=800x480, 2=1280x720, 3=1920x1080 / 30 or 60
    // fps. Unlike CarPlay -- which takes literal pixel dimensions, so it is
    // asked for the display's exact size -- the AA protocol only carries an
    // enum of standard modes, so this stays an explicit setting. Used by both
    // `android-resolution` is the legacy alias of aa-resolution.
    static inline Setting<int> aaResolution{"aa-resolution", 1, "android-resolution"};
    static inline Setting<int> aaFps{"aa-video-fps", 30};
    // Wireless Android Auto (protocol = aa-wireless): the head unit runs a
    // Wi-Fi AP and advertises the AA profile over Bluetooth; the phone joins
    // the AP and connects to the TCP server. Needs hostapd + dnsmasq + BlueZ.
    static inline Setting<std::string> wifiIface{"wifi-interface", "wlan0"};
    // The access point is run by the system (init + hostapd), not by us, so it
    // stays up when the app does not -- which is what makes it usable for
    // debugging and deployment. These four are MIRRORS of that configuration:
    // they are overwritten at start-up from the effective hostapd.conf, and the
    // Wireless UI writes changes back into that file. Setting them in a preset
    // has no effect on the radio.
    static inline Setting<std::string> hostapdConf{"hostapd-conf", "/etc/hostapd.conf"};
    static inline Setting<std::string> apRestartCmd{
        "ap-restart-command", "/etc/init.d/S44ap stop; /etc/init.d/S44ap start"};
    static inline Setting<std::string> wifiSsid{"wifi-ssid", "FastCarPlay"};
    static inline Setting<std::string> wifiPass{"wifi-passphrase", "carplay1234"};
    static inline Setting<int> wifiChannel{"wifi-channel", 6}; // 2.4GHz (ESP32)
    static inline Setting<std::string> apIp{"wifi-ap-ip", "192.168.53.1"};
    static inline Setting<std::string> btName{"bluetooth-name", "FastCarPlay"};
    static inline bool aaWireless() { return protocol.value == "aa-wireless"; }
    // Wireless CarPlay (protocol = carplay-wireless): same Wi-Fi AP + Bluetooth
    // bootstrap as aa-wireless, but for the iPhone. Reuses wifi-* / wifi-ap-ip /
    // wifi-channel above (use a 5GHz channel, e.g. 36 -- Apple requires 5GHz).
    // The MFi 3.0 auth coprocessor sits on this i2c bus/address.
    static inline Setting<std::string> mfiI2cBus{"mfi-i2c-bus", "/dev/i2c-1"};
    static inline Setting<int> mfiI2cAddr{"mfi-i2c-addr", 0x10};
    // Which codec CarPlay is offered (HEVC vs H.264) is not a setting: it is
    // derived from what this board's decoder actually supports -- see
    // video_path::preferredCodec().
    static inline bool carplayWireless() { return protocol.value == "carplay-wireless"; }
    // Wired CarPlay (protocol = carplay-wired): no Wi-Fi/Bluetooth. Brings the
    // USB-plugged iPhone to config 6, opens com.apple.carkit.service, runs the
    // iAP2 handshake, and the phone streams CarPlay over the USB-NCM link to the
    // :7000 server. Reuses the MFi chip (mfi-i2c-*) above.
    static inline bool carplayWired() { return protocol.value == "carplay-wired"; }
    // ms to wait for the phone to re-enumerate in accessory mode after the
    // AOAP switch (first connections show a consent dialog on the phone).
    static inline Setting<int> aaAccessoryTimeout{"aa-accessory-timeout", 5000};
    // Media ack window offered in the AV setup response.
    static inline Setting<int> aaMaxUnacked{"aa-max-unacked", 4};
    // Offer only ChaCha20-Poly1305 for the AA TLS session. On CPUs without AES
    // acceleration (e.g. ARM926) it is far cheaper than AES-128-GCM, which the
    // phone otherwise picks. false = normal negotiation (usually AES-128-GCM).
    static inline Setting<bool> forceChacha20{"force-chacha20", false};
    static inline Setting<int> width{"width", 1024};
    static inline Setting<int> height{"height", 576};
    static inline Setting<int> sourceFps{"source-fps", 60};
    static inline Setting<int> loglevel{"log-level", 2};

    // Device configurations section
    static inline Setting<bool> leftDrive{"left-hand-drive", true};
    static inline Setting<int> nightMode{"night-mode", 2};
    static inline Setting<int> dpi{"android-dpi", 120};

    // Application configuration section
    // How video is decoded and shown. "auto" (default) probes the board: if
    // we can become DRM master, video goes on a DRM plane with the UI on an
    // overlay plane above it; otherwise headless. Override with "hw" or
    // "headless" for bring-up or a board that misreports. See video_path.h.
    static inline Setting<std::string> videoPath{"video-path", "auto"};
    // Touchscreen for the headless DRM/DEFE path (read directly from evdev).
    // Empty device = auto-detect the first node with an absolute-position axis
    // (e.g. the GT911). swap/invert are for panel orientation calibration.
    static inline Setting<std::string> touchDevice{"touch-device", ""};
    static inline Setting<bool> touchSwapXY{"touch-swap-xy", false};
    static inline Setting<bool> touchInvertX{"touch-invert-x", false};
    static inline Setting<bool> touchInvertY{"touch-invert-y", false};
    static inline Setting<int> renderingBuffer{"rendering-buffer", 5};
    static inline Setting<int> usbQueue{"async-usb-calls", 32};
    static inline Setting<int> usbTransferSize{"usb-buffer-size", 2048};
    static inline Setting<int> usbBuffer{"usb-buffer", 128};
    // Control-channel bulk-write retry on a clean transient timeout. On the
    // F1C200s (MUSB DMA), video RX-DMA holds the shared USB FIFO (BUS_SEL=1), so
    // a PIO control write landing in that window times out with 0 bytes sent --
    // re-issuing it is safe (nothing was sent) and lands in a DMA gap. Short
    // per-attempt timeout so a collision is caught + retried in a fraction of a
    // second instead of stalling the whole AA session.
    static inline Setting<int> usbWriteTimeoutMs{"usb-write-timeout-ms", 300};
    static inline Setting<int> usbWriteRetries{"usb-write-retries", 6};
    static inline Setting<int> usbWriteRetryDelayMs{"usb-write-retry-delay-ms", 2};
    // Run the USB read threads at realtime (SCHED_FIFO 50). Correct on a
    // dedicated head unit -- it is what keeps the USB read from stalling --
    // but on a desktop it outranks the compositor, so while a session streams
    // the machine can stop responding to its own keyboard and mouse. Set
    // false when running FastCarPlay in a window alongside other work.
    static inline Setting<bool> realtimePriority{"realtime-priority", true};
    // Prefill (in queued segments) before the mixer starts consuming a freshly
    // started source: absorbs bursty delivery so the first words don't stutter.
    // Voice (mono) tolerates a longer prefill than music start-up.
    static inline Setting<int> audioDelay{"audio-buffer-wait", 2};
    static inline Setting<int> audioDelayCall{"audio-buffer-wait-call", 6};
    static inline Setting<float> audioFade{"audio-fade", 0.3};
    // The ALSA PCM the in-app mixer and the mic capture open DIRECTLY (no
    // dmix -- the mixer sums main+aux itself). "hw:0,0" on the F1C avoids
    // the plugin layer entirely; a desktop dev host can set "default".
    static inline Setting<std::string> alsaDevice{"alsa-device", "hw:0,0"};
    // Output volume, 0-100 %, saved by the screens and the volume keys; -1
    // until first changed (the card's current level is taken over).
    static inline Setting<int> volume{"volume", -1};
    // What sets it: "auto" -- the card's main playback control (Master, PCM,
    // Speaker, Headphone, ...), or a software gain when it has none --
    // "software", or the exact name of an ALSA simple-mixer control.
    static inline Setting<std::string> volumeControl{"volume-control", "auto"};
    // The ALSA control device; empty: the card of alsa-device.
    static inline Setting<std::string> volumeDevice{"volume-device", ""};
    // One volume key press or -/+ tap, in percent.
    static inline Setting<int> volumeStep{"volume-step", 5};
    // Voice latency cap: the mixer drops the OLDEST queued aux segments once
    // the backlog exceeds this, so the caller is never heard far behind the
    // live edge. Media is not capped (smoothness wins).
    static inline Setting<int> audioMaxLatencyMs{"audio-voice-max-latency-ms", 300};
    // The mixer holds the codec open across gaps shorter than this (no reopen
    // churn mid-call) and releases it after this long of full silence, so an
    // idle app leaves the codec free and spends no period wakeups.
    static inline Setting<int> audioIdleHoldMs{"audio-idle-hold-ms", 1500};
    // Software gain applied to captured mic PCM (after the DC blocker) before it
    // is sent to the phone. Integer multiplier with saturation; 1 = unchanged.
    // Measured on-board: raw speech already peaks ~70% FS, so >1 clips loud
    // speech into an inaudible square wave (the phone AGCs the rest). Tune via
    // usersettings.txt only for a genuinely quiet mic fitting.
    static inline Setting<int> micGain{"mic-gain", 1};
    static inline Setting<std::string> onConnect{"on-connect-script", ""};
    static inline Setting<std::string> onDisconnect{"on-disconnect-script", ""};
    // Settings > System: what Reboot / Shut down run once the app has closed
    // down -- the system's own commands, so init stops every service.
    static inline Setting<std::string> rebootCommand{"reboot-command", "reboot"};
    static inline Setting<std::string> shutdownCommand{"shutdown-command", "poweroff"};

    // Debug section
    static inline Setting<bool> codecLowDelay{"decode-low-delay", true};
    static inline Setting<bool> codecFast{"decode-fast", true};
    // Run the built-in screens on the overlay plane while no video plays, so
    // a source can be picked on the head unit. Selecting one persists it and
    // restarts into that protocol. `lvgl-ui` is the legacy alias.
    static inline Setting<bool> ui{"ui", false, "lvgl-ui"};

    static bool load(const std::string &filename);
    static void print();

    // User overrides, applied *after* the shipped preset. The presets ship
    // from this repo (the buildroot package tracks the branch tip), so an
    // image update rewrites them -- anything the UI changes must live here
    // instead, or it would be silently lost on the next build.
    // Path: $HOME/.fastcarplay/usersettings.txt (/root/... on the device).
    static std::string userPath();
    static bool loadUser();

    // Which user-settings file loadUser() found, reported by print().
    static std::string _userPathTried;
    static bool _userApplied;
    // Applies the value now and persists it, leaving other overrides intact.
    static bool setUser(const std::string &key, const std::string &value);


private:
    static void trim(std::string &s);
};

#endif /* SRC_SETTINGS */
