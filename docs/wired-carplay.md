# Wired CarPlay (protocol = `carplay-wired`)

Wired CarPlay to an iPhone over **plain USB** — no Carlinkit dongle, no Wi‑Fi AP,
no Bluetooth, no ESP32. The head unit brings the phone into its hidden CarPlay
USB mode (config 6), opens Apple's `com.apple.carkit.service`, runs the iAP2
identification + MFi authentication, and hands the phone a `CarPlayStartSession`
pointing at the head unit's own IPv6 on the USB‑NCM link. The phone then streams
CarPlay to the existing `:7000` server — the **same AV stack the wireless backend
uses**, so the decoder / renderer / display / touch paths are unchanged.

Proven end‑to‑end on an **iPhone 17 Pro / iOS 26.5.1**: HEVC screen video
streaming into the app over USB, plus touch/button input back to the phone.

---

## How it works (the chain)

```
iPhone (USB, config 4)
   │  ① vendor ctrl 0xC0/0x52  → reveal hidden configs (4 → 6)
   │  ② select USB config 6    → CarPlay USB mode
   ▼
config 6 interfaces:
   iface 1  ff/fe  usbmux   ──► ③ cp_usbmux: claim + mux‑TCP + usbmuxd socket
   iface 2  ff/fd  ipheth  ┐
   iface 3  02/0d  cdc_ncm ├─► kernel USB‑ethernet  → usb0 (fe80 link‑local)
   iface 4  0a     cdc_ncm ┘
   iface 5/6 02/0d+0a       (2nd NCM, unused; kernel bind() fails — harmless)
   iface 0  06/01  PTP/AFC
        │
        │  ④ libimobiledevice over the cp_usbmux socket:
        │       lockdown handshake → start com.apple.carkit.service → enable SSL
        ▼
   carkit TLS iAP2 stream
        │  ⑤ Iap2Link.negotiate() → link NORMAL
        │  ⑥ control‑session messages:
        │       StartIdentification → IdentificationInformation (USBHostTransport)
        │       RequestAuthCert/Challenge → MFi chip sign → AuthSucceeded
        │       CarPlayAvailability → CarPlayStartSession { ip = fe80::HU, port 7000 }
        ▼
   phone connects to [fe80::HU%usb0]:7000
        │  ⑦ standard CarPlay: pair‑setup / pair‑verify / /auth‑setup / SETUP / RECORD
        ▼
   HEVC (or H.264) screen video + audio + touch  ──► the app's videoStream / audio / input
```

Nothing on `:7000` is wired‑specific — it is byte‑for‑byte the CarPlay/AirPlay‑2
protocol the wireless path already speaks. The wired work only delivers frames to
`[fe80::HU]:7000` and tells the phone (via `CarPlayStartSession`) to connect there.

---

## Components

| File | Role |
|---|---|
| `src/protocol/cp/cp_usbmux.{h,cpp}` | Self‑contained C++ usbmux daemon (the `muxd.py` replacement): config‑6 reveal + select, claims **only** the usbmux interface, mux‑TCP (`MuxHost`/`MuxConn`), and a usbmuxd‑compatible UNIX socket (`MuxServer`, XML plists via libplist). Leaves the NCM interfaces to the kernel `cdc_ncm`. |
| `src/protocol/cp/cp_carplay_msg.{h,cpp}` | `buildWiredIdentification()` (USBHostTransportComponent, `car_play_interface_number = 3`) + `buildWiredStartSession()` (`wired_attributes { ip_address }` + port + pk + source_version). |
| `src/protocol/cp/cp_iap2_link.{h,cpp}` | iAP2 link layer (marker + SYN/ACK, control‑session CSMs) over any reliable fd — here the carkit TLS stream. Shared with the Bluetooth wireless‑handoff path. |
| `src/protocol/cp/cp_wired_connection.{h,cpp}` | `CpWiredConnection : IConnection` — the backend. Runs the MFi chip + `cp_server(:7000)` + the wired trigger loop (cp_usbmux → carkit → iAP2 → StartSession), feeds decoded video into `videoStream`, and translates the app's `writeQueue` (touch/buttons) into event‑channel HID. Mirrors the wireless `CpConnection`. |
| `src/protocol/cp/mfi_auth.{h,cpp}` | The MFi 3.0 auth coprocessor over i2c (challenge/response + certificate). |
| `src/protocol/cp/cp_server` + `cp_av` + `cp_control_channel` + pairing/crypto | The existing `:7000` CarPlay AV stack, reused unchanged. |

### Standalone examples (bring‑up / debugging)

```
sudo out/cp_usbmux_serve <sysfs-serial>                 # just the usbmux daemon
sudo out/cp_wired_probe  <dashed-udid> <sock> usb0 …    # handshake → reach :7000 (accept‑logger)
sudo out/cp_wired_serve  <dashed-udid> <sock> usb0 …    # full session, counts video frames
```

Build them with `make -C examples cp_usbmux_serve cp_wired_probe cp_wired_serve`.

---

## Building

The wired backend is gated behind `USE_CP_WIRED` (like the wireless one is behind
`USE_CP_WIRELESS`). It needs **libimobiledevice** and **libplist**:

```bash
sudo apt install libimobiledevice-dev libusbmuxd-dev libplist-dev   # dev headers
make USE_CP_WIRED=1 -j$(nproc)                                       # → out/app
```

The `USE_CP_WIRED=1` Makefile block adds `-DUSE_CP_WIRED` and links
`libimobiledevice-1.0` + `libplist-2.0`. Without the flag, `cp_usbmux` /
`cp_wired_connection` compile to nothing and neither library is required.

---

## One‑time host setup — run without `sudo`

The app needs three privileges; only the config‑6 switch actually requires
elevation (the kernel force‑detaches ipheth/cdc_ncm/usbmuxd on a sysfs
`bConfigurationValue` write — the userspace `USBDEVFS_SETCONFIGURATION` ioctl
can't, it `EBUSY`s while any driver holds an interface):

| Operation | Needs | Granted by |
|---|---|---|
| usbfs node (reveal / claim / bulk) | node access | `plugdev` group |
| MFi chip `/dev/i2c-1` | i2c access | `i2c` group |
| switch to config 6 | write root‑owned sysfs | `cap_dac_override` on the app binary |

Run the setup script once:

```bash
./scripts/setup-wired-carplay.sh
```

It installs:
- **`/etc/udev/rules.d/99-fastcarplay.rules`** — grants your user the iPhone
  (`05ac`) usbfs node, stops the desktop grabbing it (`ID_GPHOTO2=""`,
  `UDISKS_IGNORE=1`, `ID_MM_DEVICE_IGNORE=1`), and disables IPv6 DAD on the NCM
  net interface (so the `fe80` is usable immediately).
- **`/etc/modules-load.d/fastcarplay.conf`** — preloads `cdc_ncm` / `ipheth`
  (so `usb0` binds at cold boot, no re‑plug needed) and `i2c-dev`.
- adds you to **`i2c`** + **`plugdev`**.
- grants the app **`cap_dac_override`** so the config‑6 sysfs write succeeds.

Then **unplug/replug the iPhone** (so the udev rule + modules apply) and ensure
the MFi chip is powered:

```bash
pinctrl set 4 op dh          # GPIO4 high (this board); persist via 'gpio=4=op,dh' in config.txt
```

> **Re‑run `sudo setcap cap_dac_override+ep out/app` after every rebuild** —
> linking produces a fresh binary without the capability.

If you'd rather not use a file capability, just run the app with `sudo` — that
covers all three privileges.

---

## Running

```ini
# settings_cp_wired_pi.txt
protocol      = carplay-wired
# video-path defaults to auto: a desktop session -> SDL window + software
# decode; otherwise DRM plane + hardware decode when the chip has a block for
# the codec. The CarPlay codec offer (HEVC vs H.264) follows from that.
mfi-i2c-bus   = /dev/i2c-1
mfi-i2c-addr  = 0x10
```

```bash
./out/app settings_cp_wired_pi.txt        # as your user (after setup), no sudo
```

Plug in the **unlocked** iPhone. On the first connection tap **Trust** (a valid
pairing must exist — see below). The log should show:

```
cp-usbmux: usbmux up for … (config 6)
carplay-wired: carkit TLS iAP2 channel up, NCM usb0 = fe80::…
iAP2 link NORMAL → MFi authentication succeeded → identification accepted
CarPlayStartSession -> [fe80::…]:7000
CarPlay: phone connected the :7000 control channel → encrypted
CarPlay screen codec: HEVC/h265 → screen first frame decrypted+reframed
```

The backend reconnects on its own if the phone is unplugged/replugged.

---

## Pairing

Wired CarPlay needs a valid lockdown pair record (the same trust `idevicepair`
uses). If lockdown fails with **Invalid HostID**, the record is stale:

```bash
sudo systemctl restart usbmuxd
idevicepair pair            # UNLOCK the phone first (it refuses while locked), tap Trust
idevicepair validate        # SUCCESS: Validated pairing …
```

`cp_usbmux` reads the pair record from `/var/lib/lockdown/<UDID>.plist` (it tries
the UDID as‑is / upper / lower). libimobiledevice normalizes the serial to the
**dashed** UDID (`00008150-001C31300A63401C`), and pairing writes that dashed
file, so no manual copy is needed with `cp_usbmux`.

---

## Troubleshooting

- **`could not select config 6 (still 4)`** — the config‑6 sysfs write is being
  denied. Run the setup script (`cap_dac_override`) or run with `sudo`. (The
  ioctl path `EBUSY`s while drivers hold the device — that's expected; the sysfs
  write is the fallback.)
- **No `usb0` / `NCM … has no IPv6 link-local`** — the kernel `cdc_ncm`/`ipheth`
  didn't bind. Preload them (setup script) or rebind manually:
  `echo <bus>-<port>:6.2 | sudo tee /sys/bus/usb/drivers/ipheth/bind` and
  `…:6.3 → cdc_ncm/bind`. `ip -6 addr show usb0` should show an `fe80::` address.
- **`carkit open failed (paired? unlocked?)`** — re‑pair (above); keep the phone
  unlocked.
- **`MFi sign failed` / auth fails** — the auth chip is unpowered or on a
  different bus/addr. Check `pinctrl get 4` (GPIO4 high) and
  `i2cdetect -y 1` (chip answers at `0x10`).
- **iPhone drops off USB after idle** — usually a cable/port‑power issue or the
  phone locking the USB session. Use a good cable + powered port; replug and
  `idevicepair validate`.

---

## F1C200s notes

- Nothing to set for video: the app reads the cedrus' V4L2 capabilities, sees
  H.264-only (no HEVC), and asks the phone for H.264 automatically.
- Build with `USE_CP_WIRED=1` (and the target's `USE_CEDRUS`).
- The F1C has a USB **host** port; the same config‑6 → usbmux → carkit → NCM
  flow applies. libimobiledevice + libplist must be in the target rootfs
  (buildroot packages).
- No ESP32 / Wi‑Fi AP / Bluetooth coexistence needed — a strong argument for the
  wired path over wireless on the F1C.
