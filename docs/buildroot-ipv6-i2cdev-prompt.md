# Prompt: make IPv6 + i2c-dev hard requirements of the fastcarplay package

Paste the block below into a Claude Code session opened on the **buildroot repo**
(`/home/mtekbas/projects/f1c200s/f1c200s-linux`).

---

FastCarPlay needs two kernel features on the F1C200s that the image does not
currently guarantee. One of them is missing right now and blocks CarPlay
completely. Please make both of them explicit requirements of the fastcarplay
package rather than something that happens to be inherited.

## 1. `CONFIG_IPV6=y` — currently MISSING, hard blocker

Verified on the live board (6.6.143): `/proc/net/if_inet6` does not exist, i.e.
the kernel was built **without IPv6**.

Effect: FastCarPlay's CarPlay control server opens an `AF_INET6` socket, which
fails with `EAFNOSUPPORT`, and the app reports:

```
[CpWiredConnection] carplay-wired: failed to start :7000 server (already running?)
[CpConnection] CarPlay: failed to start :7000 / mDNS (already running?)
```

(Nothing was actually on `:7000` — the app's message was misleading and has since
been fixed upstream to name the real cause.)

This is not something that can be worked around in the app: **both CarPlay paths
hand the phone an IPv6 link-local address to connect back to.** The wired handoff
sends `CarPlayStartSession { ip = fe80::…%usb0, port = 7000 }`, and the wireless
one does the same over `wlan0`. There is no IPv4 form of that handshake. Android
Auto is unaffected (IPv4 over AOAP).

## 2. `CONFIG_I2C_CHARDEV=y` — appears satisfied, but is not declared

The MFi authentication coprocessor is reached from userspace as `/dev/i2c-N`.
On the board today `/dev/i2c-0` exists and the chip answers at `0x10`
(`i2cdetect -y 0` shows `10`), so this is currently fine — but
`CONFIG_I2C_CHARDEV` is not set in `board/lctech/pi-f1c200s/linux.fragment`, so it
is only inherited from the base defconfig and could silently disappear on a
kernel bump. Please make it explicit.

Context: the board enables only `i2c0` (TWI0, PD0/PD12), shared with the GT911
touch controller at `0x5d` — no address clash with the MFi chip at `0x10`/`0x11`.
The app is configured with `mfi-i2c-bus = /dev/i2c-0`.

## What to do

Add both symbols to `board/lctech/pi-f1c200s/linux.fragment`, each with a short
comment saying which feature depends on it (CarPlay's fe80 handoff; the MFi
coprocessor). Keep the file's existing style — it already groups options by
purpose, e.g. the "Goodix GT911 capacitive touch on I2C0 (TWI0)" block.

Then make the dependency real rather than implicit, in whichever way fits this
tree best — propose your choice and say why:

- If Buildroot can express it, add the kernel-config requirement to
  `package/fastcarplay/Config.in` (e.g. a `depends on`/`select` or a `comment`
  that warns when it is not met), and/or
- add a build-time or post-build assertion that fails loudly if the resulting
  kernel config lacks either symbol, rather than shipping an image where CarPlay
  cannot start.

`post-build.sh` in the board directory is a reasonable place for such a check if
nothing better exists — it runs with the target directory and the kernel build
available.

## Verify

After rebuilding the kernel, on the board:

```sh
cat /proc/net/if_inet6      # must exist (empty output is fine, an error is not)
ls /dev/i2c-0               # must exist
i2cdetect -y 0              # MFi chip answers at 0x10
```

Then start the app and confirm `cp-server: listening on :7000` appears instead of
the "already running?" error.

## Constraints

- Do not edit the FastCarPlay app repo.
- Keep the changes minimal and in this repo's existing style.
- Say clearly anything you could not verify without booting the board.
