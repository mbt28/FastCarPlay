# FastCarPlay

A C++ CarPlay and Android Auto receiver for small embedded Linux boards,
built for the Allwinner F1C200s (ARM926EJ-S @ 408 MHz, 64 MB RAM, no FPU).
It speaks the native protocols directly -- wireless and wired CarPlay,
wireless and wired (AOAP) Android Auto -- with hardware H.264 decoding.
No interposer dongle is involved.

![Logo](docs/images/screenshot.png)

## How it works

- **Video**: the phone's H.264 stream is decoded by the SoC's mainline
  `cedrus` engine (ffmpeg V4L2-request hwaccel) and presented on a DRM video
  plane. The app owns the display; there is no desktop, no X11/Wayland.
- **UI**: the built-in screens (source picker, settings, wireless setup with
  an on-screen keyboard) are [Slint](https://slint.dev), software-rendered
  into an RGB565 overlay plane above the video, driven by evdev touch.
- **Audio**: an in-app ALSA mixer plays music/navigation/voice directly on
  `hw:0,0` (fixed-point Opus and AAC decode -- the chip has no FPU) and
  captures the microphone for calls and Siri/Assistant.
- **Wireless modes** need a system-run Wi-Fi access point (hostapd) and
  BlueZ for the Bluetooth bootstrap. **CarPlay** (wired and wireless) needs
  an MFi 3.0 authentication coprocessor on i2c.

The complete board image (kernel, drivers, init, this app) is built by the
companion Buildroot tree: [f1c200s-linux](https://github.com/mbt28/f1c200s-linux).

## Building

This is a cross-compiled, board-only application. With the companion tree's
toolchain and sysroot (staging must contain the `slint-cpp` package for the
UI):

```
BR=/path/to/f1c200s-linux/output/host
make release \
  USE_CEDRUS=1 USE_AA_WIRELESS=1 USE_CP_WIRELESS=1 USE_CP_WIRED=1 \
  USE_SLINT=1 SLINT_COMPILER=$BR/bin/slint-compiler \
  CXX=$BR/bin/arm-buildroot-linux-gnueabi-g++ \
  CC=$BR/bin/arm-buildroot-linux-gnueabi-gcc \
  PKG_CONFIG=$BR/bin/pkg-config
```

Feature flags (compiled out entirely when unset):

| Flag | Enables |
|---|---|
| `USE_CEDRUS=1` | hardware decode + DRM display path |
| `USE_CP_WIRELESS=1` | wireless CarPlay (BlueZ/D-Bus + AP handoff) |
| `USE_CP_WIRED=1` | wired CarPlay (USB config-6 + own usbmux, libimobiledevice) |
| `USE_AA_WIRELESS=1` | wireless Android Auto (BlueZ/D-Bus + AP) |
| `USE_SLINT=1` | the on-device UI (needs slint-cpp in the sysroot) |

Without `USE_SLINT` the app builds UI-less (~630 KB stripped): video and the
session still run, sources are switched with the `carplay-source` helper or
by editing settings. Flags are not dependency-tracked -- `make clean` when
switching them.

## Configuration

The device boots `/etc/fastcarplay/settings_drm.txt`; changes made from the
on-device UI are persisted to `~/.fastcarplay/usersettings.txt`, which is
applied on top and survives image updates. [settings.txt](settings.txt) is
the full annotated reference of every key.

## Status

Working on the F1C200s, verified on-device:

- Wireless CarPlay: pairing, video + multitouch, music, calls both
  directions, Siri, navigation prompts over music, reconnect.
- Wired CarPlay (USB-NCM): session, video/touch, audio, replug.
- Wired Android Auto (AOAP): video + touch, music, voice.
- The Slint UI: source picker (persists + restart-to-apply), night/fps
  settings, Wi-Fi/Bluetooth setup with validation.

Known limits:

- Wireless Android Auto is blocked on the ESP32 (esp-hosted) SoftAP
  association; the protocol side is implemented.
- One phone at a time; no multi-device switching.

## Acknowledgement

The project is inspired by and based on great work done by other developers:

- [pycarplay by electric-monk](https://github.com/electric-monk/pycarplay)
- [carplay-receiver by harrylepotter](https://github.com/harrylepotter/carplay-receiver)
- [react-carplay by rhysmorgan134](https://github.com/rhysmorgan134/react-carplay)
- [carplay-client by rayphee](https://github.com/rayphee/carplay-client)

Licensed under GPL-3 (see LICENCE). The UI ships the
[Open Sans font](https://fonts.google.com/specimen/Open+Sans) (see
FONT_LICENCE).

## Finally

If you have questions, suggestions or problems running this, feel free to
open an issue.
