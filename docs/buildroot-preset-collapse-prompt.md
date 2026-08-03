# Prompt: update the device tooling for the new FastCarPlay presets

Paste the block below into a Claude Code session opened on the **buildroot repo**
(`/home/mtekbas/projects/f1c200s/f1c200s-linux`).

---

The FastCarPlay app repo just replaced its 13 `settings_*.txt` presets with two,
and the device tooling in this repo still selects presets by filename — so the
image will not autostart until it is updated. Please fix that.

## What changed upstream (already done, commit `d58e335` on branch `f1c200s-cedrus`)

Deleted: `settings_aa_pi.txt`, `settings_aa_pi_wireless.txt`, `settings_cedar.txt`,
`settings_cedrus.txt`, `settings_cedrus_aa.txt`, `settings_cedrus_cp.txt`,
`settings_cedrus_cp_wired.txt`, `settings_cedrus_ui.txt`,
`settings_cedrus_wireless.txt`, `settings_cp_pi.txt`, `settings_cp_wired_pi.txt`,
`settings_ui.txt`.

Now shipped:
- **`settings_drm.txt`** — head units (the app owns the screen via DRM/KMS).
  480x272 fullscreen, ALSA, LVGL UI, `force-chacha20`, F1C MUSB USB tuning, and
  the Wi-Fi AP defaults. First-boot backend is `protocol = aa-usb`.
- **`settings_desktop.txt`** — development on a desktop (SDL window). Not useful
  on the device.
- `settings.txt` — the fully-commented reference; sets nothing, not a preset.

Why the per-board/per-protocol presets went away:
1. The app now picks its video path itself (`video-path = auto`): it probes the
   V4L2 decoder nodes and DRM master, so `renderer`, `cedrus-decode`,
   `cedar-decode` and `carplay-hevc` no longer exist as settings.
2. The backend is chosen at **runtime** from the LVGL UI (Settings > Source),
   which writes `protocol` to `usersettings.txt` (`$HOME/.fastcarplay/`) and
   restarts the app. So a file per protocol is redundant.

## What to change in this repo

Everything below is under `rootfs-overlay/`.

1. **`etc/init.d/S99carplay`** — currently derives `settings_<name>.txt` from
   `/etc/carplay-preset` (defaulting to `cedrus_wireless`), falling back to
   `/etc/fastcarplay/settings.txt`. Make it launch **`/etc/fastcarplay/settings_drm.txt`**
   unconditionally. Keep the existing `/etc/carplay-autostart` opt-out, the
   `SDL_AUDIODRIVER=dummy` env, the `/tmp/carplay.log` redirect, and the
   backgrounded launch. The script also waits for `wlan0` + `hci0` when the
   preset name contains "wireless" — that test no longer works, so decide and
   implement one of: (a) always wait briefly for the radios, or (b) only wait
   when the persisted protocol in `usersettings.txt` is a wireless one. Say which
   you chose and why.

2. **`usr/bin/carplay-preset`** — it lists/sets preset files by name. Repurpose it
   to select the **protocol** instead, since that is what the user actually wants
   to switch. It should read/write the `protocol` key in the app's
   `usersettings.txt` (the same file the UI writes — find its exact path from the
   app's `Settings::loadUser`/`setUser`; it is under the app user's home) and
   accept: `aa-usb`, `aa-wireless`, `carplay-wired`, `carplay-wireless`,
   `carlinkit`. With no argument it should print the current protocol and the
   valid choices. Keep the "effective from next boot (or restart the app)"
   wording. If you think a different command name is clearer, propose it but keep
   `carplay-preset` working as an alias.

3. **`usr/bin/autorun`** — only mentions the preset concept in comments/help.
   Update the wording so it refers to the protocol selection, not preset files.

4. **`usr/bin/cma-leak-loop`** — defaults to
   `/etc/fastcarplay/settings_cedrus_aa.txt`. Point it at
   `/etc/fastcarplay/settings_drm.txt`.

5. Grep the whole repo for any other reference to the deleted filenames
   (`settings_cedrus*`, `settings_cedar*`, `settings_aa_pi*`, `settings_cp_*`,
   `settings_ui.txt`) — docs, board files, defconfig fragments, READMEs — and fix
   them. `package/fastcarplay/fastcarplay.mk` installs `settings_*.txt` by
   wildcard, so it needs no change; confirm that is still true.

## Constraints

- Do not edit the FastCarPlay app repo — it is already done.
- Keep the changes minimal and in this repo's existing shell style (POSIX `sh`,
  the scripts are BusyBox-ash).
- The F1C has no desktop, so `settings_desktop.txt` should not be referenced by
  anything on the device (it will still be installed by the wildcard; that is
  fine).
- Explain anything you could not verify without booting the board.
