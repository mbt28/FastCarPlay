# Task: bake the wireless-Android-Auto Bluetooth system setup into the F1C200s image

You are working in the **f1c200s-linux** buildroot tree (github.com/mbt28/f1c200s-linux) that
builds the SD-card image for an Allwinner **F1C200s** wireless Android Auto head unit (busybox
init, bluez 5.79, ESP32 esp-hosted for Wi-Fi+BT). The Bluetooth path is now **working** — but it
was made to work with runtime commands + init-script edits applied by hand to the running
rootfs. Those survive a reboot but **not a reflash**. Your job: make them part of the image so a
freshly flashed card boots straight into a working BT stack.

Do NOT change kernel Wi-Fi/ESP driver behavior here — the remaining Wi-Fi-SoftAP association bug
is tracked separately. This task is only the Bluetooth **system/userland** setup.

## What must be in the image

### 1. Packages / kernel (confirm already selected)
- `BR2_PACKAGE_BLUEZ5_UTILS` (bluetoothd, `hciconfig`, `rfkill`) — present.
- `BR2_PACKAGE_BLUEZ_ALSA` (the HFP Audio Gateway backend) — present.
- `BR2_PACKAGE_DBUS` (system bus) — present.
- Kernel `CONFIG_BT_RFCOMM=y` — present (required; without it bluetoothd can't serve the AA
  RFCOMM profile).
Just verify these are enabled in the defconfig.

### 2. NEW init script: `/etc/init.d/S98btstack`
Add this to the rootfs overlay (same place the existing `S99carplay`, `S43wifi`, `S44ap` live).
It must run **before** S99carplay. It brings up dbus → bluetoothd → the HFP Audio Gateway
(bluealsad, **both roles**) → unblocks/ups hci0 → sets an Audio+Telephony class of device.

```sh
#!/bin/sh
# Bluetooth stack for wireless Android Auto. Brings up dbus + bluetoothd + an
# HFP Audio Gateway (bluealsad, BOTH roles so the phone sees the standalone
# HandsFree 0x111e record it needs before starting AA) + an Audio/Telephony
# class of device. Runs before S99carplay so FastCarPlay finds a ready stack.
BLUETOOTHD=/usr/libexec/bluetooth/bluetoothd

start() {
	if ! pidof dbus-daemon >/dev/null 2>&1; then
		mkdir -p /var/run/dbus /run/dbus
		[ -s /etc/machine-id ] || [ -s /var/lib/dbus/machine-id ] || dbus-uuidgen --ensure 2>/dev/null
		dbus-daemon --system --fork 2>/dev/null
		sleep 1
	fi
	if ! pidof bluetoothd >/dev/null 2>&1; then
		"$BLUETOOTHD" --experimental >/dev/null 2>&1 &
		sleep 2
	fi
	rfkill unblock bluetooth 2>/dev/null
	hciconfig hci0 up 2>/dev/null
	if ! pidof bluealsa >/dev/null 2>&1; then
		bluealsa -p hfp-ag -p hfp-hf -i hci0 >/dev/null 2>&1 &
		sleep 2
	fi
	# Class of Device = Audio + Telephony (phone treats us as a car). Set last so
	# it wins over what bluez/bluealsad set while registering profiles. (bluez
	# main.conf `Class` only covers major/minor, not the service bits, so set it
	# here with hciconfig.)
	hciconfig hci0 class 0x600000 2>/dev/null
	:
}

case "$1" in
	start) start ;;
	stop) killall bluealsa 2>/dev/null; killall bluetoothd 2>/dev/null ;;
	*) echo "usage: $0 {start|stop}" ;;
esac
```

Key point that is easy to get wrong: **bluealsad must run with BOTH `-p hfp-ag` AND `-p hfp-hf`.**
A single `-p hfp-ag` publishes only the Audio Gateway (0x111f); the phone will NOT start Android
Auto unless it also sees a **standalone Hands-Free (0x111e)** service record, which only appears
with the HF role too. If the bluez-alsa package ships its own init script, either remove/disable
it or make it use both roles — don't let a single-role bluealsad also run.

### 3. Update the FastCarPlay autostart: `/etc/init.d/S99carplay`
Replace the preset-selection logic so it uses the wireless preset when an opt-in flag exists
(`protocol = aa-wireless`), instead of the default `carlinkit` preset:

```sh
#!/bin/sh
# FastCarPlay autostart (last rcS step). Opt-out with `autorun off`.
# Wireless Android Auto: create /etc/carplay-wireless to autostart the
# settings_<driver>_wireless.txt preset instead of the default. Needs S98btstack.
case "$1" in
start)
	[ -f /etc/carplay-autostart ] || exit 0
	drv="$(tr -dc a-z < /etc/ve-driver 2>/dev/null)"
	preset="/etc/fastcarplay/settings_${drv}.txt"
	[ -f "$preset" ] || preset=/etc/fastcarplay/settings.txt
	if [ -f /etc/carplay-wireless ] && [ -f "/etc/fastcarplay/settings_${drv}_wireless.txt" ]; then
		preset="/etc/fastcarplay/settings_${drv}_wireless.txt"
	fi
	echo "carplay: autostarting ($preset)"
	SDL_AUDIODRIVER=dummy fastcarplay "$preset" > /tmp/carplay.log 2>&1 &
	;;
stop)
	killall fastcarplay 2>/dev/null
	;;
*)
	echo "Usage: $0 {start|stop}" ;;
esac
```
Also ship the wireless preset (`/etc/fastcarplay/settings_cedrus_wireless.txt`, `protocol =
aa-wireless`) — it already exists in the image. Decide whether to ship the `/etc/carplay-wireless`
flag by default (image boots into wireless AA) or leave it opt-in (`touch /etc/carplay-wireless`).

### 4. The fastcarplay binary must be built from FastCarPlay commit `96be544` or later
This is critical and separate from the packages above. That commit (`bt: stop registering the
HFP client profile`) removes fastcarplay's vestigial `0x111e` client registration. If the image
ships an older fastcarplay, it claims the `0x111e` UUID and bluealsad fails with
`org.bluez.Error.NotPermitted: UUID already registered`, so the standalone Hands-Free record is
never published and Android Auto never starts. Make sure the FastCarPlay build feeding this image
is at `96be544`+ (branch `f1c200s-cedrus`).

## Why (one line each)
- dbus + bluetoothd: the BT stack; nothing starts them otherwise on this image.
- bluealsad both roles: publishes AG (0x111f) **and** standalone HF (0x111e) — the phone's
  "is this a car?" check needs the standalone 0x111e before it will start AA.
- CoD 0x600000: presents the unit as an Audio+Telephony device (matches a known-good reference).
- fastcarplay @96be544: stops it from blocking bluealsad's 0x111e registration.

## Acceptance test (definition of done)
On a **freshly flashed** card, after boot with no manual commands:
1. `pidof dbus-daemon bluetoothd bluealsa` all return a pid; `pidof bluealsa`'s cmdline shows
   `-p hfp-ag -p hfp-hf`.
2. `hciconfig hci0` shows `UP RUNNING`; `hciconfig hci0 class` shows `0x600000` (Audio+Telephony).
3. `bluetoothd --compat` + `sdptool browse local` shows **both** `Handsfree Audio Gateway`
   (0x111f) and a standalone `Handsfree` (0x111e), plus `Android Auto Wireless` (4de17a00).
4. A phone pairs and **reaches the Android Auto selection screen** (the app logs `bt:
   NewConnection /fcp/aa` → `aaw: ... credentials sent`).

Note: after that, the phone tries to join the ESP32 Wi-Fi SoftAP and currently **can't associate**
— that is a separate esp-hosted-ng driver/firmware bug (see docs/esp-hosted-ap-fix-prompt.md),
out of scope for this Bluetooth-system task. Reaching the AA selection screen = success here.

## Notes / environment
- busybox init runs `/etc/init.d/rcS` → `S*` scripts in name order, so `S98…` runs before
  `S99carplay`. busybox has **no `pkill`/`pgrep`** (the scripts above use `pidof`/`killall`).
- Class of Device must be set via `hciconfig` (service bits), not just bluez `main.conf`.
- Keep everything small; this is a ~36 MB-RAM target.
