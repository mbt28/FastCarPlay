# Prompt: enable the iPhone USB-NCM ethernet drivers (wired CarPlay)

Paste the block below into a Claude Code session opened on the **buildroot repo**
(`/home/mtekbas/projects/f1c200s/f1c200s-linux`).

---

Wired CarPlay was tested on the live F1C200s board and gets all the way to the
last step before failing. The remaining blocker is that the kernel has no USB
ethernet drivers, so the iPhone's USB-NCM link never appears.

## What works already (verified on the board, 2026-08-04)

```
video path: drm (DRM master + hardware decoder) hw-h264 via cedrus
[V4L2-DRM] ffmpeg v4l2-request hwaccel ready (h264 -> cedrus -> DEFE)
cp-server: listening on :7000                     <- the CONFIG_IPV6 fix works
carplay-wired ready: MFi 3.0                      <- MFi chip on /dev/i2c-0
cp-usbmux: usbmux up for 00008150 (config 6)      <- config-6 switch works
```

So the IPv6 and I2C_CHARDEV work you just did is confirmed good, and the
config-6/usbmux trigger works on MUSB.

## The blocker

```
carplay-wired: usb0 has no IPv6 link-local -- NCM link down
```
repeating every ~13s (the app retries the whole session). On the board:

```
ls /sys/bus/usb/drivers/ | grep -E 'cdc_ncm|ipheth'   -> nothing
dmesg | grep -iE 'cdc_ncm|ipheth|ncm'                 -> nothing
```

`board/lctech/pi-f1c200s/linux.fragment` currently has USB networking switched
**off** on purpose:

```
CONFIG_USB_NET_DRIVERS=y
CONFIG_USB_RTL8152=y
# CONFIG_USB_USBNET is not set
# CONFIG_USB_NET_DM9601 is not set
# CONFIG_USB_NET_AX8817X is not set
# CONFIG_USB_NET_AX88179_178A is not set
# CONFIG_USB_NET_CDCETHER is not set
# CONFIG_USB_NET_SMSC95XX is not set
```

## Why these two drivers specifically

In USB config 6 the iPhone exposes a CDC-NCM function, and it takes **both**
drivers working together. This is the sequence captured on a working reference
system (a Pi doing the same thing):

```
ipheth  <dev>:6.2: ipheth_enable_ncm: usb_control_msg: 0
ipheth  <dev>:6.2: Apple iPhone USB Ethernet device attached
cdc_ncm <dev>:6.3 usb0: register 'cdc_ncm' ... CDC NCM (NO ZLP)
```

- **`ipheth`** binds the Apple vendor interface (ff/fd) and sends the vendor
  control request that *puts the phone into NCM mode*.
- **`cdc_ncm`** then binds the CDC-NCM data interface and creates **`usb0`**,
  which gets an IPv6 link-local address. That `fe80::` address is what the app
  sends to the phone in `CarPlayStartSession`, and the phone connects back to
  `[fe80::…%usb0]:7000`. Without it there is no CarPlay AV path at all.

## What to do

Enable USB ethernet plus those two drivers in
`board/lctech/pi-f1c200s/linux.fragment` (replacing the "is not set" lines where
they conflict):

```
CONFIG_USB_USBNET=y
CONFIG_USB_NET_CDCETHER=y
CONFIG_USB_NET_CDC_NCM=y
CONFIG_USB_IPHETH=y
```

Please check the actual Kconfig dependency chain in the 6.6 tree rather than
trusting that list verbatim — `USB_NET_CDC_NCM` and `USB_IPHETH` live in
`drivers/net/usb/Kconfig` and depend on `USB_USBNET`; `USB_NET_CDCETHER` may be
required by CDC_NCM. Include whatever else the chain needs, and drop anything
that turns out to be unnecessary.

Also mirror this into the package's `LINUX_CONFIG_FIXUPS` in
`package/fastcarplay/fastcarplay.mk`, next to the existing `CONFIG_IPV6` and
`CONFIG_I2C_CHARDEV` entries, with a one-line comment saying these are what give
wired CarPlay its `usb0` link — same reasoning as the other two: the app cannot
work without them, so the package should assert it rather than hope the board
fragment stays right. Extend the post-build check the same way.

Size note: these are small drivers, but if you want to keep the default image
lean, building them as modules (`=m`) is fine as long as they auto-load on device
plug (they are USB-ID matched, so udev/mdev will load them). State which you
chose. Everything else in the fragment stays as it is — in particular leave
`CONFIG_USB_RTL8152` alone.

## Verify

Rebuild the kernel, boot the board with an iPhone plugged in, then:

```sh
ls /sys/bus/usb/drivers/ | grep -E 'cdc_ncm|ipheth'   # both present
dmesg | grep -iE 'ipheth|cdc_ncm'                     # enable_ncm + usb0 register
ip -br addr show usb0                                 # UP with an fe80:: address
```

Then run wired CarPlay and confirm it gets past the mux:

```sh
killall fastcarplay
sed 's|^protocol.*|protocol = carplay-wired|' /etc/fastcarplay/settings_drm.txt > /tmp/cpw.txt
setsid sh -c "fastcarplay /tmp/cpw.txt > /tmp/cpw.log 2>&1" < /dev/null &
grep -aE 'carkit|iAP2 link|MFi auth|identification|CarPlayStart|:7000' /tmp/cpw.log
```

Expected next lines are `carkit TLS iAP2 channel up`, `iAP2 link NORMAL`,
`MFi authentication succeeded`, `identification accepted`, `CarPlayStartSession`.

Note: run the app with `setsid … < /dev/null` when starting it from the serial
console — otherwise it gets suspended with `Stopped (tty output)` as soon as
something writes to the tty, and never reaches the trigger loop.

## Constraints

- Do not edit the FastCarPlay app repo.
- Keep changes minimal and in this repo's existing style.
- Say clearly anything you could not verify without booting the board.
