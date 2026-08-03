# Plan: unified "Android Auto" mode (wired + wireless, one session)

**Goal.** Replace the two separate sources (AA Wired, AA Wireless) with a single **Android
Auto** source whose transport is automatic — exactly how professional head units present it. The
driver picks "Android Auto"; the box figures out USB vs Wi-Fi and hands off gracefully.

**Guiding rule (from [[car-ui-design-principle]]):** minimal driver distraction. Default to the
seamless path; never drop a live session on a stray plug-in mid-drive; keep power-user controls
one level down, not on the home screen.

---

## 1. The constraint that shapes everything: one session

A phone runs **exactly one** Android Auto projection at a time. It will never run wired and
wireless at once. So "combine into one session" means **one AA mode that selects the transport and
hands off** — not two concurrent sessions.

Two hard protocol facts drive the design:

1. **Never AOAP a phone that is mid-wireless-session.** The USB accessory switch forces the
   phone's USB stack into accessory mode, which fights the running session → drop/confusion. The
   handoff must **end the wireless session first, then** AOAP.
2. **First transport to connect wins.** Advertise wireless (BT) *and* watch USB at the same time;
   whichever the phone uses becomes the session. The other is then inhibited.

### A session cannot span both transports (settled)

"Keep the Wi-Fi session open but move the data over USB" is **not possible** — a hard protocol
limit, not ours. An AA session is one logical byte stream (TLS records + GAL framing) whose keys
and channel state are bound to a single transport — one TCP socket **or** one USB bulk pipe. On
the phone side, wireless (TCP to the AA wireless service) and wired (**AOAP accessory over USB
bulk** — not USB-NCM/IP like CarPlay) are different connections to different endpoints. Plugging in
+ AOAP makes the phone **re-initiate AA from scratch over USB** (fresh TLS + service discovery). So
any switch is a real reconnect (~3-5 s); the bytes *are* the session.

What we *can* do, and what this plan uses:
- **Keep the Wi-Fi AP + BT bootstrap infrastructure alive** across the switch (only the *session*
  migrates), so unplugging returns to wireless quickly.
- **Hide the seam:** freeze the last video frame + a "Switching to USB…" overlay during the
  reconnect, instead of a black screen.

## 2. Architecture

Your `AaConnection` is already transport-agnostic — it runs the whole GAL session (TLS, service
discovery, AV/input/sensor) over an `AaTransport`. Today the transport is fixed for the
connection's lifetime and chosen in `makeConnection()`. The change is to make **transport
acquisition pluggable** and add an orchestrator.

### Transport provider

Introduce an `AaTransportProvider`:

```
class AaTransportProvider {
  // Blocks until a phone connects on some transport, or `active` goes false.
  // Returns the connected transport (ownership), or null on shutdown.
  virtual std::unique_ptr<AaTransport> acquire(std::atomic<bool>& active) = 0;
  virtual void inhibit(bool) {}   // stop offering while a session runs
};
```

- **`UsbProvider`** — the current `waitForAccessory` / AOAP scan, returns an `AaUsbTransport`.
- **`WirelessProvider`** — owns the Wi-Fi AP + BT bootstrap (today's `AaWireless` bring-up),
  accepts the TCP connection, returns an `AaTcpTransport`.
- **`CombinedProvider`** — runs both concurrently, returns whichever connects first, and
  `inhibit()`s the loser while the session runs.

`AaConnection::mainLoop` changes from `_transport->open()` to `provider->acquire()`. The
single-transport modes become thin providers (USB-only / wireless-only) — the existing behavior,
just refactored behind the provider.

### One-session gate + inhibit

While a session is live, `CombinedProvider` inhibits the *other* transport:
- USB session active → stop BT advertising / don't accept new TCP. (AP may stay up or drop — see
  §6 coexistence.)
- Wireless session active → USB is **charge-only**: detect a plug, but do **not** AOAP.

On session end, both providers resume.

## 3. Plug-in behavior — narrowed to one behavior

**Plug USB during a wireless session → always switch to wired.** No policy setting; one code path.

```
             ┌─────────────┐   phone connects (USB or TCP)   ┌──────────────┐
   idle ────▶│  listening  │────────────────────────────────▶│   session    │
             │ USB + wless │                                  │ (one xport)  │
             └─────────────┘                                  └──────┬───────┘
                    ▲                                                 │ USB plugged
                    │ session ends / unplug                           │ while WIRELESS
                    │                                                 ▼
                    │                        ┌──────────────────────────────────────┐
                    └────────────────────────│ 1. end wireless session (close TCP)   │
                                             │ 2. show "Switching to USB" + freeze    │
                                             │ 3. AOAP the phone -> new wired session │
                                             └──────────────────────────────────────┘
```

- Handoff **order is mandatory**: end the wireless session first (close TCP → phone drops
  wireless), *then* AOAP. AOAP-ing a live wireless session is the conflict from §1.
- During the ~3-5 s reconnect: **freeze last frame + "Switching to USB…" overlay** (§1) so it is
  not a black screen.
- **Unplug during wired** → return to idle → wireless re-advertises. If the AP was kept up, resume
  is fast; on the F1C200s we drop it (see §6), so resume is slower but the ESP32 is not stressed.

Accepted tradeoff: someone who plugs in *only to charge* still gets the switch. That is the chosen
behavior.

## 4. New menu structure

The two AA sources collapse into one. Transport becomes an **Android Auto** settings sub-screen —
professional pattern, and the default "Auto" needs no interaction.

```
Home
  ▶ Resume                     (only when a session is backgrounded)
  ⚙ Settings

Settings
  Source            Android Auto  ›     → Source
  Android Auto               ›          → Android Auto settings   (NEW)
  Night                     Auto
  Video                    30 fps
  Icons                  material
  Debug                      Off
  ↻ Restart now                         (only when a change is staged)
  ← Back

Source                                  (radio: one held at a time)
  Android Auto          ✓
  Dongle
  ← Back

Android Auto  (settings)                (NEW — replaces the old "Wireless" screen)
  Transport             Auto            → cycles Auto / Wired / Wireless
  Wi-Fi name       FastCarPlay          → on-screen keyboard
  Wi-Fi password   ••••••••             → on-screen keyboard
  Bluetooth name   FastCarPlay          → on-screen keyboard
  ← Back
```

Notes:
- **Source** drops from 3 rows to 2 (Android Auto, Dongle) — one less decision for the driver.
- **Transport = Auto** is the default and covers everyone. **Wired** / **Wireless** are explicit
  overrides for power users (force wired when the AP is flaky; force wireless to stop USB take-over).
- No "On plug-in" row — the switch-to-wired behavior is fixed (§3), so there is nothing to choose.
- The old `screen_wireless` becomes `screen_aa_settings` — same keyboard editors, plus the
  Transport cycle row. Wireless credential fields move here where they belong.

## 5. Settings changes

| Setting | Values | Default | Notes |
|---|---|---|---|
| `protocol` | `carlinkit` \| `aa` | `carlinkit` | `aa` = the combined mode |
| `aa-transport` | `auto` \| `wired` \| `wireless` | `auto` | replaces the split; `auto` races both |

(No `aa-on-plug` — switch-to-wired on plug is fixed behavior, §3.)

**Back-compat:** keep `aa-usb` / `aa-wireless` as accepted `protocol` values that map to
`protocol=aa` + `aa-transport=wired`/`wireless`, so existing presets and `usersettings.txt` keep
working. `settings_cedrus_aa.txt` etc. need no edits.

## 6. Coexistence & power (F1C200s reality)

- **ESP32 SoftAP is still the blocker** for real wireless (the association bug). The combined mode
  can be built and the **wired path validated now**; the wireless leg + handoff are validated once
  the AP works (or on the dev Pi with BT + hostapd).
- **BT/Wi-Fi coexistence:** running the AP + BT advertising continuously stresses the ESP32
  (documented hci0 drops). Policy: bring the AP/BT up when **idle or wireless-preferred**; while a
  **USB session** is active, drop BT advertising and optionally the AP to relieve the co-processor.
  This is a tuning knob, not a correctness issue.
- **Charging while wireless:** `charge` mode keeps the phone powered over USB with no AOAP — the
  common, desirable case.

## 7. Phases

**P1 — provider refactor + combined auto-select (wired-first validation).**
`AaTransportProvider`; USB/wireless/combined providers; `AaConnection` uses `acquire()`. `protocol
= aa`, `aa-transport = auto`. One-session gate + inhibit. *Milestone:* on the board, `protocol=aa`
connects a plugged phone as wired; the wireless bootstrap runs in parallel without breaking wired.

**P2 — switch-to-wired handoff + seam cover.**
Detect USB plug during a wireless session → end wireless, freeze last frame + "Switching to USB…"
overlay, AOAP, start wired. *Milestone (once AP works):* wireless session + plug USB → wired, no
double session, no wedge, no black screen.

**P3 — UI restructure.**
Merge Source to Android Auto / Dongle; `screen_aa_settings` (Transport, On plug-in, Wi-Fi/BT
fields). Back-compat setting mapping. *Milestone:* pick Android Auto, set Transport/plug policy,
sweep clean at all panel sizes.

**P4 — testing + stress.**
Wired connect/disconnect/replug storms; wireless connect (when AP works); handoff both directions;
exit/resume across the handoff; one-session invariant under races (plug during wireless connect,
unplug mid-handshake). Reuse `tools/thread-cpu.sh` and the `examples/` harness.

## 8. Risks & open questions

- **AOAP-during-wireless is the sharp edge.** The USB watcher must detect a plug *without*
  switching to accessory mode while wireless is live. Needs care: scan/enumerate only, AOAP only
  on the deliberate handoff.
- **"Same phone?" on plug-in** — we can't easily prove the plugged phone is the one on wireless.
  Assume yes (the normal case); a handoff to a *different* phone is an edge we log and treat as a
  fresh session.
- **ESP32 coexistence** may make "AP always up" impractical; the idle-only-AP policy mitigates it
  but needs on-board measurement.
- **Real plug-in behavior is phone/version-dependent** (S21 vs Pixel, Android version). P4 must
  test on real hardware; the design copes with either phone choice because we never force a second
  session.
- **Scope:** P1+P3 deliver the visible feature (one "Android Auto", auto wired/wireless). P2 (live
  handoff) is the harder, more phone-dependent part — could ship after P1/P3 with `charge`-only.

## 9. Recommendation

Build **P1 + P3** first (combined auto-select + the merged menu), default `aa-on-plug = charge`,
validating the wired path on the board now. Add **P2** (prefer-wired handoff) once wireless works
end-to-end, since it can only be meaningfully tested then. This ships the professional
single-"Android Auto" experience immediately and defers only the phone-dependent handoff.
