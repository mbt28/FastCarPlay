# The on-device UI

The built-in screens -- source picker, settings, wireless setup -- are
[Slint](https://slint.dev), software-rendered into the DRM overlay plane
above the video plane. One fixed 800×480 RGB565 surface; touch, plus a
Bluetooth keyboard or remote (see `docs/bluetooth-input.md`).
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
- **Keys** (`SlintOsd::key`, fed by `src/key_input.cpp`): a FIFO drained at
  the start of each tick. Up/Down are injected as Backtab/Tab, so Slint's own
  focus traversal and scroll-into-view move an amber ring over the rows;
  Enter activates the focused row (each `Row` has a `FocusScope`). Back/Esc,
  Backspace and Left call the exported `back()` function and Home sets the
  screen directly, so neither depends on focus. While the editor or a dialog
  is open, keys go straight to its callbacks (typed characters ->
  `osk-key`). In the editor the arrows move a ring over the on-screen
  keyboard (`Osk` global: layout, shift/symbols, ring position, `move()` /
  `activate()`), Enter presses the ringed key or, with no ring, saves; typing
  hides the ring. In a dialog the arrows ring its buttons (`prompt-focus`),
  Enter presses the ringed one or accepts. Rows never take focus from a tap (`focus-on-click: false`),
  so touch use never shows a ring; after a screen switch the ring is
  re-seeded on the first row only if a key was used in the last 10 s
  (returning from video keeps the focused row). A held Backspace keeps
  deleting only if it was pressed in the editor, not when it closed a
  dialog over it.
- **Fonts**: embedded at compile time (`embed-for-software-renderer`);
  Open Sans at `src/ui/slint/fonts/font.ttf`. No fontconfig, no filesystem
  fonts on the target.
- **Build**: `slint-compiler` turns `app.slint` into `src/autogen/
  app_window.h` (see the `USE_SLINT` block in the Makefile). The static
  `libslint_cpp` comes from the buildroot `slint-cpp` package.

## Screens and behaviors

- **Home**: `<source> - <status|ready>` header; a double-height Resume row
  only while a connected session waits behind the UI; Settings.
- **Settings**: Back on top (easiest reach), Volume, Source, Wireless,
  Bluetooth (builds with Bluetooth support only; only four rows fit above
  the fold: the volume, then connectivity), Night
  (day/night/auto cycle), Video (30/60 fps), and Restart now -- which
  appears only once a staged change needs it. Staged changes never drop a
  live session; the header says "restart to apply".
- **Volume** (Settings row with -/+, and the volume / mute keys of a
  Bluetooth keyboard or remote, whatever is on screen): applies at once and
  is saved 2 s after the last change. `src/audio_volume.cpp` sets it through
  the card's own playback control when it has one -- ALSA's simple mixer,
  any Linux card: Master or PCM; else the card's only output amp
  (Headphone, Speaker, Line Out...); else a shared digital control
  (Digital, DAC, Playback); or `volume-control` -- mapped through its dB
  scale (cubic, as alsamixer and PulseAudio do), muted by its switch or a
  silent bottom step; otherwise a ramped gain in AudioMixer does the work.
  The first start takes over the card's current level without writing it.
  Mute is never saved.
- **Source**: radio list of the four backends; selecting persists
  `protocol` to usersettings and stages a restart.
- **Wireless**: SSID / passphrase / BT name, each opening a
  full-screen editor with an on-screen keyboard (letters, shift, symbols).
  WPA2 limits are enforced before saving (SSID 1-32, passphrase 8-63
  bytes); invalid input shows a red hint and keeps the editor open; Cancel
  always exits. The AP config is written to hostapd and the AP restarts;
  the Bluetooth name is a usersetting applied at start-up.
- **Bluetooth** (screen 4): Add device / Stop searching (a 30 s discovery,
  refused during a wireless phone session), the paired keyboards and remotes
  (blue = connected; tap for its device screen), then the found ones (tap to
  pair). Phones are never listed: they pair from their own settings.
- **Bluetooth device** (screen 5): Connect / Disconnect and Forget (asks
  first). Forgetting the device on screen returns to the list.
- **Pairing dialog**: replaces the screen (it wins over the editor -- pairing
  has a deadline). Shows the code to type on the keyboard (40 px), the
  progress BlueZ reports, or a yes/no question; Cancel always exits. When
  video takes the screen the dialog stays open behind it, ready to finish
  when the UI is back (BlueZ's timeouts end it otherwise).
- **Navigation state** lives in `Bridge.screen` (0 home, 1 settings,
  2 source, 3 wireless, 4 bluetooth, 5 bt device) so C++ can follow it
  (discovery runs only while 4/5 is visible) and drive it (Home key).

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
- **Lists sync by id, never reset**: device rows are updated in place,
  erased or appended (`syncRows`). A model reset destroys every row
  instance -- keyboard focus is lost and the whole list repaints.
- **Strings from outside are ASCII**: the software renderer only has the
  glyphs the compiler embedded -- its default printable-ASCII set plus every
  string literal in `app.slint` -- so device names are sanitised to
  printable ASCII in C++ before they reach the UI. The default set lacks the
  backtick; the symbols keyboard's `` ` `` key is what embeds it.
