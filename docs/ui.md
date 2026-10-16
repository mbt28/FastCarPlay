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
  the start of each tick. Up/Down are injected as Backtab/Tab, so Slint's
  own focus traversal and scroll-into-view move an amber ring over the rows;
  Enter activates the focused row (each `Row` has a `FocusScope`). Back/Esc,
  Backspace and Left call the exported `back()` function and Home sets the
  screen directly, so neither depends on focus. The one exception is the
  Volume row: its `-` and `+` report their focus (`stepper-focus`), so Right
  on `-` moves to `+` and Left on `+` moves to `-` (Left on `-` is still
  Back). While the editor or a dialog is open, keys go straight to its
  callbacks (typed characters -> `osk-key`). In the editor the arrows move a
  ring over the on-screen keyboard (`Osk` global: layout, shift/symbols,
  ring position, `move()` / `activate()`), Enter presses the ringed key or,
  with no ring, saves; typing hides the ring. In a dialog the arrows ring
  its buttons (`prompt-focus`), Enter presses the ringed one or accepts.
  Rows never take focus from a tap (`focus-on-click: false`), so touch use
  never shows a ring; after a screen switch the ring is re-seeded on the
  first row only if a key was used in the last 10 s (returning from video
  keeps the focused row). The same happens when the row with the ring goes
  away while its screen stays (the Resume row, a Bluetooth device): a
  removed row never reports losing focus, so such rows report holding it
  (`ring-row`). A held Backspace keeps deleting only if it was pressed in
  the editor, not when it closed a dialog over it.
- **Rotary knob** (a scroll wheel on a Bluetooth device; its click is
  Enter): one detent is `Next`/`Prev`. On the lists that is Tab/Backtab,
  wrapping at the ends; in the editor the ring walks every key in reading
  order (Cancel, the key rows, the bottom row ending in OK) and wraps
  (`Osk.step()`); in a dialog it cycles the buttons. Every screen starts
  with a Back row, so turn + press alone reaches everything.
- **Fonts**: embedded at compile time (`embed-for-software-renderer`);
  Open Sans at `src/ui/slint/fonts/font.ttf`. No fontconfig, no filesystem
  fonts on the target.
- **Build**: `slint-compiler` turns `app.slint` into `src/autogen/
  app_window.h` (see the `USE_SLINT` block in the Makefile). The static
  `libslint_cpp` comes from the buildroot `slint-cpp` package.

## Screens and behaviors

- **Home**: `<source> - <status|ready>` header with the phone's link icons
  at its right -- USB or Wi-Fi while a session runs over it, Bluetooth while
  a phone is connected over Bluetooth (line drawings, `Path`); a
  double-height Resume row only while a connected session waits behind the
  UI; Settings.
- **Settings**: Back on top (easiest reach), Audio, Source, Wireless
  (only four rows fit above the fold: audio, then connectivity), Night
  (day/night/auto cycle, below), Video (screen 8, below), System, and
  Restart now -- which appears only once a staged change needs it. Staged changes
  never drop a live session; the header says "restart to apply".
- **Video** (screen 8): Frame rate (30/60 fps, needs a restart), and
  Camera when a V4L2 capture device is there (`camera-device`, or the first
  one found -- the F1C200s TV decoder for a composite reversing camera); its
  value is why the camera last could not run.
- **Camera view** (screen 9): the camera full screen on the video plane, no
  copy -- `src/camera.cpp` exports its capture buffers as dma-bufs and
  `drm_display::showCamera()` scans them out (NV12 in contiguous memory; the
  display engine scales 720x576 to the panel), the phone's frames held off
  the plane meanwhile (`holdVideo`). Over it, a settings column at the
  right edge (180 px wide, as tall as its rows: `CameraPanel`) is the only
  window of the overlay; it hides 5 s after the last input, and a tap on
  the picture hides it at once.
  - Two pages. The menu: Back (leaves the view), one row per input (the
    one on screen is filled; "No signal" when it has none), Standard
    (Auto/PAL/NTSC) and Picture. Picture: Back (to the menu), then the
    levels the device has -- Brightness, Contrast, Saturation, Hue,
    Sharpness, Chroma gain, Chroma enhance -- and - / + under them. Every
    change applies at once. Choosing an input is live; a standard the
    stream is not in restarts it (half a second of black). Auto follows the
    input, checked once a second, as does "No signal".
  - Each input keeps its own settings (`camera-0` .. `camera-3`, raw
    control values; one left out is at its default -- never the other
    input's), saved 2 s after the last change and when the view closes.
  - Touch: Back, an input, Standard and Picture act on a tap; a level is
    ringed by a tap and changed with - / +. With the column hidden a touch
    only brings it back.
  - Keys and knob: while hidden anything brings the column back (Back
    leaves the view). Turn / Up / Down move the ring; Enter or a click acts
    on the ringed row, and on a level starts editing (turning then changes
    it; Enter or Back ends it); Left / Right change a level or the
    standard; Back goes up a page, then leaves. An input or the standard
    never changes by turning.
  - The TV decoder's burst gate, comb filter and blue screen controls are
    never touched (noise, memory corruption on the bench). A camera that
    cannot start or stops goes back to Video saying why; short of memory
    beside a phone session, the comb filter (2 MiB) stays off.
- **System** (screen 7): Reboot and Shut down. Each asks first, in the
  dialog below; for a knob or Enter the ring starts on Cancel, and a press
  with no ring only shows it, so a double press cannot take the head unit
  down. The question does not wait behind the video. Then the app closes
  down cleanly (phone session, audio, the saved volume) and `main` runs the
  system's own command -- `reboot-command` / `shutdown-command`, `reboot` and
  `poweroff` by default -- so init stops every service. Should the command
  fail, or the system not go down within 60 s, the app starts again rather
  than leave the screen dark.
- **Night** applies at once, to a running session too. CarPlay: Night
  darkens it, Day keeps it light even after sunset (the display's
  appearance, `uiAppearanceUpdate`), Auto leaves it to the phone, which
  darkens at sunset where it is; `setNightMode` goes along, and `/info`
  carries the same at connect. Android Auto: its night-mode sensor; it
  follows nothing by itself, so Auto shows day.
- **Audio** (screen 6): Volume with -/+ and Mute. The volume / mute keys of
  a Bluetooth keyboard or remote work whatever is on screen. A change
  applies at once, shows the volume bar for 2 s and is saved 2 s after the
  last one; mute is never saved. `src/audio_volume.cpp` sets it through the
  card's own playback control when it has one -- ALSA's simple mixer, any
  Linux card: Master or PCM; else the card's only output amp (Headphone,
  Speaker, Line Out...); else a shared digital control (Digital, DAC,
  Playback); or `volume-control` -- mapped through its dB scale (cubic, as
  alsamixer and PulseAudio do), muted by its switch or a silent bottom step;
  otherwise a ramped gain in AudioMixer does the work. The first start takes
  over the card's current level without writing it.
- **Volume bar**: 480x64, bottom centre, over the screens and over live
  video (in the camera view 180x48 at the foot of the settings column, so
  the overlay window never grows). Over video it is a window of the overlay plane
  (`drm_display::uiShowWindow`) that the display engine composites: no video
  frame is touched, and while it is up no commit happens at all -- one
  non-blocking commit shows it, one hides it (a pending video flip only
  means retrying on the next loop turn). It is rendered before it shows and
  hidden before it is redrawn, so no frame of a screen lands on the video.
- **Source**: radio list of the four backends; selecting persists
  `protocol` to usersettings and stages a restart.
- **Wireless**: SSID / passphrase / BT name, each opening a
  full-screen editor with an on-screen keyboard (letters, shift, symbols).
  WPA2 limits are enforced before saving (SSID 1-32, passphrase 8-63
  bytes); invalid input shows a red hint and keeps the editor open; Cancel
  always exits. The AP config is written to hostapd and the AP restarts;
  the Bluetooth name is a usersetting applied at start-up. Then Bluetooth
  (builds with Bluetooth support only), the device screens below.
- **Bluetooth** (screen 4, Back returns to Wireless): Add device / Stop
  searching (a 30 s discovery, refused during a wireless phone session), the
  paired keyboards and remotes (blue = connected; tap for its device
  screen), then the found ones (tap to pair). Phones are never listed: they
  pair from their own settings.
- **Bluetooth device** (screen 5): Connect / Disconnect and Forget (asks
  first). Forgetting the device on screen returns to the list.
- **Dialog**: replaces the screen (it wins over the editor -- pairing has a
  deadline). Shows Bluetooth's prompt -- the code to type on the keyboard
  (40 px), the progress BlueZ reports, or a yes/no question -- or else the
  System screen's question; Cancel always exits. When video takes the
  screen a Bluetooth dialog stays open behind it, ready to finish when the
  UI is back (BlueZ's timeouts end it otherwise).
- **Navigation state** lives in `Bridge.screen` (0 home, 1 settings,
  2 source, 3 wireless, 4 bluetooth, 5 bt device, 6 audio, 7 system,
  8 video, 9 camera view) so C++ can follow it (discovery runs only while
  4/5 is visible, the camera only while 9 is) and drive it (Home key).

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
