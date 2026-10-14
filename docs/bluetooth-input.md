# Bluetooth keyboards and remotes

FastCarPlay pairs and uses Bluetooth HID devices -- keyboards, media remotes,
game pads -- alongside the phone. Keys drive the phone (CarPlay knob/media,
Android Auto keys) or, while a FastCarPlay screen is up, the screens
themselves, including text entry in the editor.

```
src/bluetooth/bt_manager.{h,cpp}   the one pairing agent + HID device management
src/key_input.{h,cpp}              evdev reader for Bluetooth keyboards/remotes
src/ui/slint/app.slint             Bluetooth screens, pairing dialog, focus ring
src/ui/slint_osd.cpp               keys -> UI, manager snapshot -> UI
```

Platform side (f1c200s-linux): bluetoothd is built with the `input` (classic
HID) and `hog` (HID over GATT) plugins; `bt on` loads `uhid` and
`hid_generic` before starting bluetoothd. A connected device appears as a new
`/dev/input/eventN` (bus `BUS_BLUETOOTH`) and disappears when it disconnects.

## The manager

`bt::Manager` runs in **every** protocol mode, started before the phone
connection and stopped after it. It owns a private system-bus connection
(a `bt off` / dbus-daemon restart cannot take the app down) and one thread
that blocks in `poll()` when there is nothing to do. Every D-Bus call inside
its loop is asynchronous, so agent requests keep being answered while a
`Pair()` is in flight; UI calls only queue commands.

The bus may not exist yet: on the image dbus-daemon runs only while
Bluetooth is on, and in wired modes the app starts before S45aa-stack's
`bt on`. The manager then waits for `/var/run/dbus/system_bus_socket`
(inotify, no polling), connects when it appears, and after a `bt off` /
`bt on` cycle connects again; meanwhile Settings shows Bluetooth as Off.
bluetoothd restarts are followed the same way (`NameOwnerChanged`).

Adapter policy: never powered down or reset (a connected keyboard survives
everything). In wireless modes the AA/CarPlay modules keep the adapter
discoverable for the phone, as before; in wired modes the manager makes it
non-discoverable. Pairable stays on -- without it even our own `Pair()` would
not store a key.

## Pairing flow

1. Settings > Wireless > Bluetooth > **Add device**: a 30 s discovery (no
   filter, so classic and discoverable LE devices both show). Only HID
   devices are listed. Discovery stops on Stop, after 30 s, when the Bluetooth screens
   are left or the UI is hidden, before pairing, and is refused (and
   stopped) while a wireless phone session is on the shared ESP32 radio:
   from the moment the phone connects over Bluetooth for the handoff,
   through ONLINE and CONNECTED.
2. Tap a found device: discovery stops and `Device1.Pair()` starts. That
   device becomes the **pairing target** for this window.
3. A keyboard asks the user to type a code: the dialog shows it large
   ("Type the code on the keyboard, then press Enter"), with the progress the
   keyboard reports. Legacy keyboards that ask for a PIN get a random 6-digit
   one; remotes get "0000". A device that shows a code itself gets a yes/no
   confirmation. The code dialog closes by itself once the key is stored
   (`Bonded`); `Pair()` still waits for the service search after that.
4. On success the device is marked **Trusted** (classic reconnects then skip
   authorization) and, for classic devices that did not connect on their
   own, connected once. Until `Pair()` answers, Connect / Disconnect of that
   device wait ("Busy") -- a Disconnect would cancel the service search and
   leave `Pair()` unanswered.
5. **Forget** (device screen, confirmed first) calls `Adapter1.RemoveDevice`:
   it disconnects, unpairs and deletes the stored key. The keyboard itself is
   not told (BlueZ's uhid mode sends no virtual-cable unplug), so to use it
   again put it back into pairing mode and add it again.

Reconnects need no app action: a bonded classic keyboard pages the head unit
when it wakes (the adapter keeps page scan on for bonded devices), and BlueZ
auto-connects bonded LE devices when they advertise.

## Agent rules

One agent, capability `DisplayYesNo`, registered as the default agent.
`KeyboardDisplay` was rejected: on BR/EDR the kernel sends it as
`DisplayYesNo` anyway, and on LE it only adds cases where the head unit
would have to type a passkey -- breaking the phones' prompt-free pairing.
With `DisplayYesNo`, keyboards still get MITM-protected passkey entry
(we display, they type).

A device is **HID** when its Class of Device is Peripheral, its Icon is
`input-*`, its LE Appearance is a HID one, its UUIDs contain HID
(0x1124) or HID-over-GATT (0x1812), or it has `org.bluez.Input1` -- unless
its Class of Device is Phone or Computer: those stay phones even when they
also offer a HID role (a phone running a "Bluetooth keyboard" app pairs
like any phone), and their HID channel is still refused by the
`AuthorizeService` rule below, which goes by UUID.

| Request | Pairing target | Other HID device | Anything else (phones) |
|---|---|---|---|
| RequestConfirmation | yes/no dialog | rejected | accepted |
| RequestAuthorization | accepted | rejected | accepted |
| DisplayPasskey | code dialog | disconnected (cannot be refused) | ignored |
| DisplayPinCode | code dialog | -- | -- |
| RequestPinCode | random PIN (keyboards) / "0000" | rejected | rejected |
| RequestPasskey | rejected (unreachable) | rejected | rejected |
| AuthorizeService (HID 0x1124, HOG 0x1812) | accepted | rejected | rejected |
| AuthorizeService (other UUIDs) | accepted | accepted | accepted |

"Accepted" for phones holds in the wireless modes only. Wired modes
(carplay-wired, aa-usb) ran no agent before this manager, so BlueZ refused
every phone pairing and untrusted service request there; the agent keeps
that (`Rejected`), so a phone bonded in an earlier wireless session cannot
bring up e.g. HFP and take its call audio to a head unit that renders none.

Trusted (bonded) devices never reach `AuthorizeService`. BlueZ never asks
about HID over GATT at all (it only requires the bond), so for LE devices
the pairing decision is the only gate -- and only the head unit can open
an LE connection (it does not advertise), i.e. only a device paired from
the UI or bonded before. Phone requests are
answered immediately: the agent has a single request slot in BlueZ, and a
pending one makes any other pairing fail. The pairing window ends 10 s after
`Pair()` succeeds, or when it fails, is cancelled or the device is removed.

`Cancel` closes the dialog. A user Cancel aborts the in-flight `Pair()` with
`CancelPairing` -- but only until the key is stored: after that,
`CancelPairing` no longer cancels but silently unpairs in the controller
while bluetoothd still reports the device paired. So the code dialog closes
at `Bonded`, and a Cancel that loses the race against the key exchange ends
in `RemoveDevice` of the half-made bond (`Pair()` then succeeds, as BlueZ
ignores the failed service search). After a Cancel, further requests from
that device are refused, and Forget during a pairing ends it. A dialog that
video covers stays open: it is back when the UI returns, and the pairing's
own timeouts end it otherwise.

## Key map

Only Bluetooth input devices are read (`EVIOCGID` bus `BUS_BLUETOOTH`), and
each is grabbed (`EVIOCGRAB`): no other kernel handler sees its keys, which
matters because Magic SysRq is enabled on the image -- without the grab,
Alt+SysRq+B on a paired keyboard would reboot the head unit. A device counts
when it has keys this table uses, a hat, or a scroll wheel -- so a
keyboard's touchpad half ("i4 Mouse") is read for its wheel and wheel click,
which together act as a rotary knob with a centre press. Its pointer motion
and left/right buttons are ignored. Dials (`REL_DIAL`) are not read: their
counts are not detents (a Surface Dial makes 3600 a turn).

| Key | To the phone | In FastCarPlay's screens |
|---|---|---|
| Up / Down | CarPlay: knob up / down; Android Auto: one knob turn, as the wheel (repeats) | focus previous / next row; in the editor and dialogs, move the ring (repeats) |
| Left / Right | knob left / right; in Android Auto a jump between areas (repeats) | Left = Back on the lists, except on the Volume row: Right on `-` moves to `+`, Left on `+` moves to `-` (Left on `-` is still Back); in the editor and dialogs, move the ring (repeats) |
| Enter, keypad Enter, OK, Select | select (held while held) | activate the focused row; in the editor press the ringed key (no ring: save); in a dialog press the ringed button (no ring: accept) |
| Space | -- | activate the focused row; types a space in the editor |
| Esc, Back | back | back (cancels a dialog or the editor) |
| Backspace | -- | delete a character in the editor, else back |
| Home, Home page | home | home screen |
| Volume up / down (repeats), Mute | the head unit's volume | the head unit's volume |
| Scroll wheel (one step per detent) | knob rotation (CarPlay) / rotary controller (AA) | next / previous: rows, the editor's keys, a dialog's buttons |
| Wheel click (middle button) | select, as Enter | activate, as Enter |
| Play/Pause, Play, Pause | media play/pause | (phone) |
| Next / Previous song | next / previous track | (phone) |
| Voice command, Search, Assistant | Siri / assistant | (phone) |
| letters, digits, punctuation | -- | typed into the editor (US layout) |

Both phones have a native rotary knob, and the wheel drives it: one detent
is one knob step (CarPlay's knob wheel, Android Auto's rotary controller;
clockwise -- the wheel turned towards you -- is the next item). It moves the
focus through the current list or area. Android Auto takes the D-pad as the
knob's nudge, which only jumps between areas (the rail, the content, the
cards), so there Up/Down turn the knob too and Left/Right jump between
areas. In CarPlay the arrows are the knob's nudges, which also pan a map.

Typing keys -- letters, digits, Space, Backspace -- are never shortcuts on
the phone: in a CarPlay search field they would select or go back instead of
typing. They do not type there either: the phone only gets the HID devices
the head unit declares (touchscreen, knob, media, telephony), and none of
them is a keyboard.

In the editor the arrows move an amber ring over the on-screen keyboard
(Cancel, the keys, OK), so a remote without letters can type: the first
arrow shows the ring on the first letter, Enter/OK presses the ringed key.
Typing on a keyboard hides the ring again, so its Enter saves.

Arrow repeats are generated by the reader (first after 400 ms, then every
150 ms -- 120 ms on screen), not the kernel's 33 ms, so a held key cannot
flood the phone's write queue (Android Auto shares it with video
acknowledgements). Wheel turns go to the phone one detent per report, at
most one every 30 ms; a faster spin keeps up to 4 steps and drops the rest.
Select is the only key sent as press/release: CarPlay holds its knob button
until the release. A key belongs to the route it was pressed on: when the
screen hands over to video (or back), or the device disappears, a held
select is released on the phone and the rest of that press is swallowed. The
same happens to everything a device holds when the kernel reports lost
events (`SYN_DROPPED`, its queue overflowed while the reader was not
scheduled): a lost release must not leave a key repeating.

## Testing on the bench

Board prerequisites: `bt status` shows bluetoothd running;
`lsmod | grep -E 'uhid|hid_generic'` lists both; `/dev/uhid` exists.

1. **Pair a classic keyboard** through Settings > Wireless > Bluetooth >
   Add device: the code dialog appears, typing it plus Enter on the keyboard
   completes pairing, the keyboard shows as connected, and the log shows
   `key-input: + /dev/input/eventN`.
2. **Pair a BLE keyboard** (if available) the same way.
3. **Keys**: arrows move the amber ring and Enter activates on every screen;
   Esc/Backspace goes back; in the Wi-Fi name/password editor, typing works
   and Enter saves, and the arrows reach every on-screen key plus Cancel and
   OK (Enter presses the ringed one). With CarPlay/AA projecting, arrows/Enter
   drive the phone's focus, media keys work, letters do nothing.
4. **Wheel** (a keyboard's or mouse's scroll wheel): turning walks the ring
   through every row, the editor's keys and a dialog's buttons, wrapping at
   the ends; the click activates. With CarPlay/AA projecting it moves the
   phone's focus through lists, towards you = down.
5. **Reconnect**: power-cycle the keyboard, press a key -- it reconnects
   with no UI action. Reboot the board -- same; in a wired mode the log
   shows `bt-mgr: no system bus yet ... -- waiting for it`, then
   `system bus is up` once S45aa-stack has run `bt on`.
6. **Forget** removes it from the list; the keyboard no longer connects.
7. **Negative test**: with "Add device" NOT running, put a keyboard into
   pairing mode and try to pair from the keyboard side: it must be rejected
   (log: the agent rejects the HID request).
8. **Regression**: wired CarPlay, wireless CarPlay and wireless Android Auto
   still pair and run; idle and in-session CPU/RAM unchanged (measure with
   `/proc/<tid>/sched`).

## Image-side notes (f1c200s-linux)

- Nothing has to wait for the bus: the app copes with dbus-daemon starting
  after it (wired modes) and with `bt off` / `bt on` cycles.
- Optional: `/etc/bluetooth/input.conf` `UserspaceHID=persist` keeps a
  keyboard's evdev node across disconnects (fewer hotplug events).
- Optional, ESP32 coexistence: a bonded LE HID device that is switched off
  makes the kernel passive-scan at 50% duty (60/30 ms) indefinitely;
  `[LE] ScanIntervalAutoConnect`/`ScanWindowAutoConnect` in `main.conf`
  (e.g. 0x0100/0x0012) trade reconnect speed for radio time.
- `bluetoothctl` is installed: running it with `default-agent` would take
  the default agent from the app and bypass these rules until restart.
