# Blueprint: on-device UI (LVGL + EEZ Studio) behind the `Interface` seam

**Goal.** A touch UI for FastCarPlay — source picker (AA wired / AA wireless / Carlinkit dongle,
CarPlay later) plus basic settings — rendered on the F1C200s OSD plane and on desktop/Pi.

Two non-negotiable properties, in this order:

1. **Simplicity** — the smallest change that works. No new DRM code, no compositor, no input
   plumbing, and **zero CPU when the UI is not on screen**.
2. **Customizable** — screens are designed **visually**, in a **free and open-source** editor, by
   anyone, on a host PC.

**Status:** M0 (toolchain + footprint gate) and M1 (first screen rendering) are **done and
verified on hardware** — see §7. M2 (source picker) is next.

---

## 1. Toolchain decision: LVGL + EEZ Studio

| Component | License | Role |
|---|---|---|
| **LVGL 9.3.0** (`third_party/lvgl`, pinned to the release tag) | **MIT** | The runtime library, compiled into the firmware |
| **EEZ Studio 0.28.0** | **GPL v3, free** | Host-side visual editor; generates C into `src/ui/generated` |

**Why not LVGL's own Editor.** LVGL the *library* is MIT and free forever, but its Editor is part
of **LVGL Pro**: the Community tier is free only for **non-commercial** use, and commercial use is
**$20,000 per product**. Separately, LVGL's XML parser/loader was **removed from MIT core in
Jan 2026** (PR #9565, "licensing concerns"), roadmap issue
[#9663](https://github.com/lvgl/lvgl/issues/9663) still open. Building the customization story on
either would be a strategic contradiction for open, maintained firmware.

**EEZ Studio has no such restriction.** GPL v3 (matching FastCarPlay), no tiers, no seat limits,
no field-of-use restriction — GPL explicitly permits commercial use. Envox disclaims ownership of
generated output ("The user owns the `.eez-project` file and all the source code generated"), and
Flow-generated code is explicitly MIT. Licensing risk assessment: **negligible for this project**,
because FastCarPlay is already GPL-3.0, so even the most conservative reading lands on the same
license. The genuine obligation to be deliberate about is GPLv3 §6 (installation information on
locked-down consumer hardware) — which follows from FastCarPlay's own license, not from EEZ.

**Rule: never let the editor become load-bearing for the *product*, only for its *design*.** The
app must stay buildable and customizable without it.

## 2. Why this seam is nearly free

`drm_display` already does the hard part ([drm_display.h](../src/drm_display.h)): an **ARGB8888
overlay plane** above the hardware video plane (per-pixel alpha blended by the DE backend — "OSD
over live video is free"), an offscreen ARGB surface, and `uiPresent()` / `uiHide()` (the plane is
disabled when idle, so it costs no scanout bandwidth).

Crucially, **both render paths hand out an `SDL_Renderer*`**:

- `renderer = sdl` (desktop / Pi): the window's renderer,
- `renderer = drm` (F1C200s): `drm_display::uiRenderer()`, a *software* renderer targeting the
  offscreen ARGB surface flipped onto the overlay plane.

So **one LVGL backend built on `SDL_Renderer` serves both targets**, and the F1C200s path needs no
new DRM code. The F1C200s has no GPU; LVGL is pure software rendering, which is what it is for.

## 3. Resolution strategy (aspect-band design)

FastCarPlay must run on panels from 400×234 to 1280×480. The naive reading is "six layouts". It
isn't — look at the aspect ratios:

| Resolution | Aspect | Band |
|---|---|---|
| 400×240, 800×480 | 1.667 | standard |
| 1024×600 | 1.707 | standard |
| 400×234 | 1.709 | standard |
| 480×272 | 1.765 | standard |
| **1280×480** | **2.667** | **wide** |

**Five of six sit in a 1.67–1.77 band — a ~6% spread.** That is effectively *one* aspect ratio at
different pixel counts. Only 1280×480 (8:3) is genuinely different. So this is **one layout plus
one special case**, not six.

### The five rules

1. **Percentage-first, never absolute.** Author with `%` and `content` units inside **flex**
   containers; absolute px only for hairlines and icon padding. Renders **natively crisp at every
   resolution** — no upscale blur — and uses all the pixels of a 1024×600 panel.
   EEZ supports this directly: every widget has `leftUnit`/`topUnit`/`widthUnit`/`heightUnit`
   settable to `px` / `%` / `content`, and flex flow/grow/align are version-mapped for 9.3.0.
2. **Two aspect bands, not six resolutions.**
   - **standard** (1.5–2.0) → one screen set, covers five of six natively
   - **wide** (≥ 2.0) → a variant for 1280×480, e.g. two columns. A full-width row on 8:3 is
     ~1280 px long: visually absurd and touch-hostile.

   Selected at runtime from `Settings::width/height`.
3. **Font tiers by height.** LVGL fonts are bitmaps and cannot scale continuously. Compile three
   sizes and pick by panel height (≤272 small, ≤480 medium, >480 large). ~10–30 KB per size, so
   keep the charset tight.
4. **Fixed row height + scroll — never shrink-to-fit.** This protects the small panels: pure
   percentages would shrink rows to ~28 px on 400×234, below a safe touch target (~44 px). Fix the
   row height, show ~4 rows on small panels, **scroll** for the rest. Bigger panels reveal *more*
   rows, not fatter ones.
5. **Scale-fallback for anything unhandled.** Render at the nearest reference and scale the
   texture. Softer, never broken. Cheap on both targets — SDL scales in `RenderCopy`, and the DRM
   plane API already separates `SRC_W/H` from `CRTC_W/H`.

**Design reference: 800×480** — the midpoint, exactly 5:3 (matching two targets), and percentages
scale cleanly both up (1024×600) and down (400×234).

### Verified empirically

`tools/ui-sweep.sh` renders every supported size and captures each one. Run against the initial
absolutely-positioned starter screen (label at `left=356, top=232`), it reproduces the whole
failure mode in one contact sheet: **clipped off-screen with scrollbars** at 400×234 and 400×240,
**pushed into the bottom-right corner** at 480×272, **correctly centered only at 800×480** (its
design resolution), and **drifting off-centre** at 1024×600 and 1280×480. That is the argument for
rule 1, in pixels.

## 3b. Icons

**Format is forced by the target.** ThorVG/vector and the PNG/GIF decoders are off (too heavy for
ARM926), so icons are **font glyphs**, not SVG or bitmaps. That is also the cheapest option by a
wide margin — measured, for 6 icons at 3 sizes:

| | Icon font (4bpp) | 24×24 ARGB bitmaps |
|---|---|---|
| Compiled size | **3.2 KB** | ~110 KB |
| Recolour at runtime | **yes** (it is text colour) | no |
| Needs an image decoder | **no** | yes (disabled) |

**Themes work by indirection: fixed codepoints, swapped font.** Screens name a *slot*
(`icons::ICON_USB`), never a glyph. Slots are fixed Private Use Area codepoints (U+F001…), and each
theme maps those same codepoints to its own glyphs — so a theme change is one font swap and no
screen changes. `icon-theme` selects it:

- **`builtin`** — LVGL's bundled FontAwesome subset (62 symbols incl. USB/WIFI/BLUETOOTH/SETTINGS).
  Free, always available, drawn by the body font. The fallback, and what ships if nothing is built.
- **`material`** — **Material Symbols (Apache-2.0)**, subset by `tools/make-icon-font.sh` to just
  the named slots. Unknown themes fall back to `builtin` with a warning rather than failing.

`tools/make-icon-font.sh` resolves icon *names* against the `.codepoints` file Google ships with
the font (rather than hardcoding hex that rots on update), remaps each onto its PUA slot with
`lv_font_conv`, and emits `src/ui/icons_material_{14,20,28}.c` matching the §3 font tiers.

Icons are monochrome — they take the label's text colour, which is what makes theming and night
mode free. **Brand marks (Android Auto / CarPlay logos) are deliberately excluded:** they need
colour bitmaps and carry trademark terms independent of any icon licence.

Other GPL-3.0-compatible sets, should a second theme be wanted: Phosphor (MIT, 6 weights —
a theme system by itself), Lucide (ISC), Tabler (MIT), Bootstrap Icons (MIT). Avoid Font Awesome
Free as a primary set: CC BY 4.0 attribution plus a Pro upsell.

## 4. The binding contract (design ↔ app)

EEZ Flow's generated `ui.c` exposes exactly the seam we want:

```c
native_var_t native_vars[] = { ... };   // app -> UI  (state the screens display)
ActionExecFunc actions[]   = { ... };   // UI -> app  (what buttons do)
void ui_init();                          // build screens / start flow
void ui_tick();                          // eez_flow_tick() + tick_screen()
```

Anyone can restyle, relayout or rewire screens in EEZ without touching C++, as long as they use
these names. Planned surface:

**Native variables:** `protocol` (int), `status` (string), `phone_name`, `wifi_ssid`, `wifi_pass`,
`bt_name`, `night_mode` (bool).

**Native actions:** `select_protocol(int)` — `0`=carlinkit, `1`=aa-usb, `2`=aa-wireless, leaving
`3`/`4` for CarPlay — plus `toggle_night_mode()` and `apply_and_restart()`. One parameterized
action rather than three keeps the contract small and extends for free.

**The only hand-written glue is `src/ui/ui_bridge.cpp`**, implementing these against `Settings::`.

### Persistence

**The UI must never write `settings_cedrus_aa.txt`.** That file ships from this repo via the
buildroot package (which auto-tracks `f1c200s-cedrus`), so the next image build would clobber user
choices — and it is heavily commented documentation. Instead write only changed keys to
**`$HOME/.fastcarplay/usersettings.txt`** (`/root/.fastcarplay/...` on the device), loaded *after*
the preset. Shipped presets stay pristine and updatable; user choices survive image updates.
`ISetting::asString()` already exists, so the writer is small — the *loader* needs an overlay pass.

## 5. Files

```
third_party/lvgl/           vendored LVGL 9.3.0 (MIT) -- library only
src/ui/lv_conf.h            LVGL config for this target (see §6)
src/ui/lvgl_osd.{h,cpp}     LVGL bound to an SDL_Renderer + pointer input
src/ui/generated/           EEZ Studio output -- GENERATED, never hand-edited
src/ui/ui_bridge.cpp        (M2) native vars/actions implemented against Settings::
ui.eez-project              the EEZ project (design source of truth)
tools/ui-sweep.sh           render + capture every supported panel size
```

Two rules that keep this sane: `src/ui/generated/` is **regenerated, never hand-edited**, and the
app touches it only through `ui_bridge.cpp`, so a redesign cannot ripple into app logic.

**Workflow.** EEZ Studio has **no headless/CLI build** and ships **x86_64-only for Linux** (arm64
builds are macOS-only); an arm64 Linux build can be made from source (`npm install && npm run
build`), which is how it runs on the Pi5 here. Either way: **design + Build in the GUI → commit
`src/ui/generated/` → targets only compile.** No EEZ on any target.

> **Trap, hit once already:** EEZ's `destinationFolder` had defaulted to `src\ui`, and the Build
> **overwrote that directory**, destroying the LVGL tree vendored there (untracked, so
> unrecoverable from git). Generated output now goes to `src/ui/generated/` and LVGL lives in
> `third_party/`, outside anything EEZ writes to.

## 6. `lv_conf.h` and build

Generated from LVGL's official template, with: `LV_COLOR_DEPTH 32` (ARGB8888, matching the
overlay plane — no format conversion), `LV_USE_OS 0` (we drive `lv_timer_handler`), `LV_USE_LOG 0`,
`LV_USE_EVDEV 1`, `LV_MEM_SIZE 48K`, and ThorVG / vector graphics / PNG / GIF / QR / FreeType /
TinyTTF / FFmpeg / demos **off**. Rendering is **partial** with a ~1/10-screen buffer (480×272 →
~52 KB), so LVGL redraws only dirty rectangles.

Build: `make USE_LVGL=1`. `src/ui` is excluded from the source globs unless the flag is set, so a
UI-less build pays no flash or RAM. LVGL builds with `-std=gnu99` (its evdev driver needs
`O_CLOEXEC`) and `-w` (third-party).

## 7. Milestones

### M0 — toolchain proof + footprint gate ✅ **done**
Cross-compiled for the real target (arm926 / armv5te, soft-float, `-Os`): LVGL **419/419 files,
zero errors**; `eez-flow.cpp`, `ui.c`, `screens.c`, `styles.c`, `images.c` all clean — including
under release flags (`-fno-rtti`, `-ffast-math`, `-fvisibility=hidden`), which was a real risk.

Linked footprint (`--gc-sections`, not archive size):

| Build | text (flash) | data | bss (RAM) |
|---|---|---|---|
| LVGL only | 216 KB | 788 B | 64 KB |
| LVGL + generated UI + **Flow** | 548 KB | 5.8 KB | 77 KB |
| **Flow's marginal cost** | **+333 KB** | +5 KB | **+12 KB** |

**Verdict: keep Flow.** +333 KB flash and +12 KB RAM against an SD-card image and ~36 MB free RAM
is comfortably affordable, and it is a *fixed* cost (measured on an essentially empty project), so
it will not scale badly with screens. Flow also makes generated code explicitly MIT, and it lets
users change *behavior* visually — the strongest version of the customization goal. Keep the
native-action surface small so Flow does **routing, not logic**.

**Still unmeasured: CPU per `eez_flow_tick()`** on the 408 MHz core. Static analysis can't answer
it. Mitigated structurally — we only tick while the UI is visible.

### M1 — display + input bring-up ✅ **done**
`LvglOsd` (LVGL → `SDL_Renderer`, sized at runtime from `Settings::width/height`, pointer input),
`lvgl-test` harness (`loopLvglTest()`) that renders the generated screens with no connection,
decoder or audio. **Verified on the Pi5: the EEZ screen renders at 60.7 fps** (474 frames /
7.8 s), no errors.

### M2 — source picker ← **next**
Main screen (AA wired / AA wireless / Carlinkit + Settings) using the §3 rules; `select_protocol`
+ `protocol`/`status` native vars; `$HOME/.fastcarplay/usersettings.txt` override loader;
apply-by-restart (`S99carplay` respawns).
*Accept:* switch source from the screen, survives reboot, preset file untouched, sweep clean at
all six sizes.

### M3 — settings page ✅ **done**
Night mode (day/night/auto), video frame rate, debug overlay, icon theme, Back — plus a shared
style layer (`ui_style`) and a screen navigator (`ui_screens`) so the two screens cannot drift.

Every change persists immediately to `usersettings.txt`. Settings that only reach the phone at
session setup (night mode, frame rate) are marked **restart required**: the header says so and a
"Restart now" row unhides, rather than restarting under the user. Icon theme applies **live** via
a screen rebuild (a different icon set is a different font, so labels must be recreated).

Navigation is deferred through `lv_async_call`: a row's click handler runs *on* the screen being
replaced, so tearing it down inline would free the object mid-event.

**Verified** by scripted clicks (`lvgl-test-click`, which accepts a `x,y;x,y` sequence): picker →
Settings → toggle Night mode → value updates to "Day", "restart to apply" appears, "Restart now"
unhides, `night-mode = 0` lands in `usersettings.txt`. Swept at 400x234 / 480x272 / 800x480 /
1024x600 — on the smallest panel the fifth row scrolls instead of shrinking, which is rule 4
doing its job.

### M4 — AA wireless page ✅ **done**
Wi-Fi name, Wi-Fi password and Bluetooth name, edited through an on-screen
keyboard overlay (`lv_keyboard` + `lv_textarea`), reached from Settings.

**Validated before saving:** WPA2 fixes the passphrase at 8-63 characters and the SSID at 1-32,
and hostapd simply refuses to start outside those. An invalid edit keeps the editor open with a
red hint instead of persisting something that would silently stop the unit advertising.

**Encoder-operable:** the editor hands the focus group to the keyboard alone (in edit mode), so
rotating steps through its keys rather than the rows hidden behind it; closing rebuilds the screen,
which puts the rows back in the group.

Two layout notes worth keeping: the editor sets `LV_OBJ_FLAG_IGNORE_LAYOUT`, because the screen is
a flex column that would otherwise place it *after* the rows instead of over them; and changes are
marked restart-required, since the AP and BT name are configured at start-up.

**Verified** by a scripted three-click flow (Settings -> Wireless -> Wi-Fi name -> keyboard OK):
value validated and persisted as `wifi-ssid`, editor closed, header switched to "restart to
apply", and encoder focus returned to the first row.

### M5 — F1C200s integration
Drive `LvglOsd` from `loopDrm()` on the OSD plane; input from evdev/GT911; **measure the Flow tick
and total UI CPU**; trim `lv_conf.h` against the measured result.

## 8. TODO

- [ ] Wide-band (1280×480) two-column layout — or ship scale-fallback first if 1280×480 is rare
- [ ] Three font tiers wired to panel height
- [ ] Open the UI **over live video** (needs an input router so touches don't also reach the phone
      — the `TouchInput` sink refactor); trigger while connected
- [ ] Encoder / button navigation (`LV_INDEV_TYPE_ENCODER`)
- [ ] Live protocol switch without a process restart
- [ ] Current-value read-back next to `set`/`toggle` items; switch / slider / dropdown rows
- [ ] Dynamic lists: Wi-Fi scan, BT device list, pairing flow
- [ ] Migrate home / toast / debug from the SDL `Interface` to LVGL, then **drop SDL2_ttf** and the
      hand-rolled renderer — one UI stack instead of two
- [ ] Optional: runtime-editable menu *content* (no rebuild) layered over the EEZ-designed template
- [ ] i18n / UTF-8 font subsetting
- [ ] Additional icon themes (Phosphor / Lucide) — build to the same PUA slots, no screen changes
- [ ] **F1C200s hardware JPEG**: no use for icons (JPEG has no alpha and is lossy on flat art), but
      potentially worth it for **album art** from CarPlay/AA, boot splash and backgrounds — all
      photographic and opaque. First verify the mainline cedrus driver actually exposes JPEG decode;
      upstream covers MPEG-2/MPEG-4/H.264/H.265/VP8, and it may only exist in the BSP libcedarc.
- [ ] CI: run `tools/ui-sweep.sh` per commit so `src/ui/generated/` can't drift from `ui.eez-project`

## 9. Risks

- **Two UI stacks during the transition** (SDL `Interface` + LVGL) — mitigated by keeping them
  modal (never drawing concurrently). Accepted deliberately to keep the MVP small.
- **Generated-code drift** — `src/ui/generated/` must never be hand-edited; enforce in CI.
- **Flow tick CPU on ARM926** — unmeasured; M5 gates it. Fallback: turning Flow off is a project
  setting, screens survive, wiring moves to C++.
- **EEZ licensing** — negligible here (GPL v3 ↔ GPL-3.0), but keep the app buildable without the
  editor so the *product* never depends on it.
- **evdev double-delivery** — LVGL's evdev driver and `TouchInput` can both read the GT911. Safe
  only while the UI is disconnected-only; do not open it over video until the input router exists.

## Sources
- [lvgl/lvgl#9663 — XML license and roadmap](https://github.com/lvgl/lvgl/issues/9663)
- [LVGL Pro](https://lvgl.io/pro) · [pricing](https://lvgl.io/pro/pricing)
- [EEZ Studio](https://github.com/eez-open/studio)
