# The on-device UI

The built-in screens -- source picker, settings, wireless setup -- are
[Slint](https://slint.dev), software-rendered into the DRM overlay plane
above the video plane. One fixed 800×480 RGB565 surface, touch-only input.
(The LVGL predecessor and the multi-panel plan it followed live in git
history: `docs/lvgl-ui-blueprint.md` before the `native` branch.)

## Architecture

```
src/ui/slint/app.slint      the screens, declarative; all behavior goes
                            through `export global Bridge`
src/ui/slint_osd.{h,cpp}    the facade: slint platform glue + renderer;
                            the ONLY C++20 translation unit
src/ui/ui_bridge.{h,cpp}    app-side state the screens bind to
src/ui/osd.h                selects the toolkit; defines USE_OSD_UI
```

- **Properties flow app → UI, callbacks flow UI → app.** `slint_osd`'s tick
  pumps `Bridge` properties *on change only* (every overlay commit costs
  real milliseconds on the 408 MHz core) and handles the callbacks
  (persist, validate, restart). Validation and persistence stay in C++ --
  the `.slint` file holds layout and visual state only.
- **Rendering**: `SoftwareRenderer` with `RepaintBufferType::ReusedBuffer`
  into a persistent full-frame buffer (750 KB). The buffer always holds the
  complete current frame, so a partial repaint flushes only the dirty rows
  (`drm_display::uiWriteRect`) and a backend transition just pushes the
  whole buffer again (`invalidate()`).
- **Input**: evdev touch, recorded from the touch thread, dispatched into
  Slint on the loop thread (Slint is single-threaded). A tap shorter than
  one 16 ms tick still delivers a press+release pair.
- **Fonts**: embedded at compile time (`embed-for-software-renderer`);
  Open Sans at `src/ui/slint/fonts/font.ttf`. No fontconfig, no filesystem
  fonts on the target.
- **Build**: `slint-compiler` turns `app.slint` into `src/autogen/
  app_window.h` (see the `USE_SLINT` block in the Makefile). The static
  `libslint_cpp` comes from the buildroot `slint-cpp` package.

## Screens and behaviors

- **Home**: `<source> - <status|ready>` header; a double-height Resume row
  only while a connected session waits behind the UI; Settings.
- **Settings**: Back on top (easiest reach), Source, Night (day/night/auto
  cycle), Video (30/60 fps), Wireless, and Restart now -- which appears only
  once a staged change needs it. Staged changes never drop a live session;
  the header says "restart to apply".
- **Source**: radio list of the four backends; selecting persists
  `protocol` to usersettings and stages a restart.
- **Wireless**: SSID / passphrase / Bluetooth name, each opening a
  full-screen editor with an on-screen keyboard (letters, shift, symbols).
  WPA2 limits are enforced before saving (SSID 1-32, passphrase 8-63
  bytes); invalid input shows a red hint and keeps the editor open; Cancel
  always exits. The AP config is written to hostapd and the AP restarts;
  the Bluetooth name is a usersetting applied at start-up.

## Design rules (inherited, still binding)

- **Driver first**: minimise distraction and effort in every decision.
  Rows are full-width targets; state is shown, not implied; anything
  dangerous (restart) is explicit and hidden until meaningful.
- **Fixed row height, scroll for the rest** -- never shrink rows to fit.
  80 px rows / 8 px gaps / 20 px text at 800×480; a touch target never goes
  below ~44 px.
- **Change-aware updates everywhere**: an unchanged screen must cost
  nothing (measured: unconditional per-tick label writes once kept the
  main thread at 26%).
- **RGB565 only**: the facade refuses any other overlay format rather than
  render garbage.
