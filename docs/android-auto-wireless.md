# Wireless Android Auto on the F1C200s

This is the checklist for running **wireless** Android Auto (`protocol =
aa-wireless`) on the F1C200s. It reuses the entire wired AA stack (framing,
TLS, service discovery, video/audio/input channels) — wireless only changes
*how the phone connects*: a Bluetooth bootstrap hands the phone the head unit's
Wi-Fi credentials + TCP endpoint, and from there the session runs over a TCP
socket instead of USB.

```
phone pairs over Bluetooth
   -> aaw RFCOMM handshake: HU sends its AP IP + TCP port, then the AP credentials
   -> phone joins the HU's Wi-Fi AP
   -> phone connects TCP 5277  ->  normal GAL session (same as wired)
```

## 0. Hardware / prerequisites

- **Wi-Fi + Bluetooth co-processor.** The reference board uses an **ESP32 in
  esp-hosted-ng** over SPI (Wi-Fi `wlan0`) + UART (Bluetooth `hci0`). Any adapter
  giving a standard `wlanX` (AP-capable via hostapd) + `hciX` works.
- **2.4 GHz caveat.** The ESP32 AP is **2.4 GHz only**. Some phones *require*
  5 GHz for wireless AA — those will pair over Bluetooth but refuse to join the
  2.4 GHz AP. Test your phone; a 5 GHz-capable adapter avoids this.
- Wireless AA does **not** use the USB port, so it stays free.

## 1. Image (buildroot) requirements

Wireless needs userspace the wired image doesn't. Add to the buildroot config:

| Need | Package | Why |
|---|---|---|
| Runtime | **hostapd** | runs the head unit's Wi-Fi AP |
| Runtime | **dnsmasq** | DHCP for the phone on the AP |
| Runtime | **dbus** + **bluez5_utils** | `dbus-daemon` + `bluetoothd` for the BT bootstrap |
| Build | **dbus + bluez dev in staging** | headers/`.pc` to cross-compile the BT code |
| Runtime | **rfkill** | the app unblocks Wi-Fi/BT at start |

The BT stack (`bluetoothd`, `libdbus`, `libbluetooth`) may already be present;
verify the **staging sysroot** has `dbus-1.pc` and `bluetooth/rfcomm.h`, and that
`hostapd`/`dnsmasq` are in the target rootfs.

## 2. Cross-build

```sh
BR=/home/mtekbas/projects/f1c200s/kernel7.1.2/f1c200s-linux/output/host
cd FastCarPlay
make clean
make USE_CEDRUS=1 USE_AA_WIRELESS=1 \
  CXX=$BR/bin/arm-buildroot-linux-gnueabi-g++ \
  CC=$BR/bin/arm-buildroot-linux-gnueabi-gcc  \
  PKG_CONFIG=$BR/bin/pkg-config
$BR/bin/arm-buildroot-linux-gnueabi-strip -o out/app-f1c200s out/app
```

`USE_AA_WIRELESS=1` links `-ldbus-1 -lbluetooth`. Without it, `protocol =
aa-wireless` is unavailable and the app falls back with a log warning.

## 3. Settings

Start from `settings_drm.txt` and change the protocol + add the wireless
block:

```
protocol = aa-wireless

# Wi-Fi AP the phone joins (2.4 GHz for the ESP32).
wifi-interface  = wlan0
wifi-ssid       = FastCarPlay
wifi-passphrase = carplay1234
wifi-channel    = 6
wifi-ap-ip      = 192.168.53.1

# Bluetooth device name the phone pairs to.
bluetooth-name  = FastCarPlay
```

Video/decode/audio settings are the same as the wired cedrus profile.

## 4. Runtime prerequisites on the board

The bootstrap talks to `bluetoothd` over the system D-Bus, so both must be
running before the app starts:

```sh
dbus-daemon --system            # if not already up
/usr/libexec/bluetooth/bluetoothd &
```

(The app itself runs `rfkill unblock` and powers the adapter.) Run the app as
**root** — hostapd, the D-Bus BlueZ calls, and `ip`/`rfkill` need it.

## 5. First run

```sh
./app-aa settings_drm.txt
```

On the phone, go to Android Auto's wireless setup (or just pair to the new
Bluetooth device "FastCarPlay"), accept pairing. Watch the serial console for:

```
[AaBluetooth] bt: adapter 'FastCarPlay' powered, pairable + discoverable
[AaBluetooth] bt: registered profile 4de17a00-... (server)
[AaWifi]      wifi: AP 'FastCarPlay' up on 192.168.53.1 (<bssid>, ch 6)
[AaBluetooth] bt: NewConnection ... (fd N)          <- phone connected RFCOMM
[aa_aaw]      aaw: RFCOMM handshake starting
[aa_aaw]      aaw: info request, sending Wi-Fi credentials
[AaTcpTransport] TCP: phone connected from 192.168.53.x   <- joined AP + TCP
[AaConnection]   Version handshake done -> TLS established -> Phone connected
[AaConnection]   RX ch 3 id 0x0000 ...                <- video streaming
```

From `Phone connected` on, it is the same session as wired — video on the DEFE,
audio, touch.

## 6. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| App warns "aa-wireless needs a USE_AA_WIRELESS build" | Rebuild with `USE_AA_WIRELESS=1`. |
| `bt: can't connect to system bus` | `dbus-daemon --system` not running. |
| `bt: set Powered failed` / adapter stays down | `bluetoothd` not running, or rfkill hard-block; check `rfkill list`, `hciconfig`. |
| Phone doesn't see "FastCarPlay" when scanning | adapter not discoverable — check the `bt:` log; ensure only our agent is default. |
| Pairs over BT but never joins Wi-Fi / no `TCP: phone connected` | 2.4 GHz rejected by the phone (needs 5 GHz), wrong SSID/passphrase, or hostapd/dnsmasq not running (`ps`, check `hostapd -dd`). |
| `wifi: hostapd failed to start` | hostapd missing or the driver doesn't support AP mode on `wlan0`. |
| Reaches TLS then drops before discovery | shared HU certificate rejected — same note as the wired path (`aa_cert.h`). |
| Video stutters | arm926 throughput — same tuning as the wired doc (it holds 30 fps wired). |

## 7. Testing without the board

Two host-side harnesses build with `USE_AA_WIRELESS`:

- `examples/aa_aaw_test` — socketpair round-trip of the RFCOMM credential
  handshake (`make aa_aaw_test`).
- `examples/aa_bt_test` — runs the Bluetooth D-Bus service against a live BlueZ
  and makes the host discoverable (`make aa_bt_test && sudo ../out/aa_bt_test`,
  then `bluetoothctl show`).

## Protocol reference

- AA RFCOMM UUID `4de17a00-52cb-11e6-bdf4-0800200c9a66`, channel **8**; a
  hands-free profile (`0000111e-…`) is also advertised so the phone offers AA.
- RFCOMM frame: `[len u16BE][msgId u16BE][protobuf]`.
- Handshake: VersionRequest(4) → VersionResponse(5) → StartRequest(1){ip,port} →
  InfoRequest(2) → InfoResponse(3){ssid,pass,bssid, security=WPA2, ap=static};
  ping(8)/pong(9) keepalive. TCP server on port **5277**.
