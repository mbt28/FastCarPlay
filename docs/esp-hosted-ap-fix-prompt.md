# Task: fix esp-hosted-ng AP-mode so Wi-Fi clients can associate to the ESP32 SoftAP

You are working on the **esp-hosted-ng** Wi-Fi stack for an Allwinner **F1C200s** board:
the Linux kernel driver (`esp32_spi`, SPI transport for Wi-Fi + UART for BT) plus the
**ESP32 slave firmware**. This board is a wireless **Android Auto** head unit: it must run a
2.4 GHz WPA2 **SoftAP** (`wlan0`) that a phone joins after a Bluetooth handshake. Everything
else works — Bluetooth pairing, the AA handshake, the phone reaching the "Android Auto"
screen — **the only remaining blocker is that no client can associate to the ESP32 AP.**

## Exact symptom (this is the bug to fix)

`wlan0` is brought up as an AP with hostapd (`driver=nl80211`, hw_mode=g, ch 6, WPA2-PSK/CCMP).
The AP beacons and the client (Samsung Galaxy S21, and any laptop) sees the SSID and
authenticates, but **association never completes.** hostapd `-dd` shows:

```
wlan0: STA <mac> IEEE 802.11: authentication OK (open system)
wlan0: STA <mac> IEEE 802.11: authenticated
wlan0: STA <mac> IEEE 802.11: association OK (aid 1)
wlan0: STA <mac> IEEE 802.11: did not acknowledge association response
```

So: auth OK → hostapd assigns aid 1 → **`did not acknowledge association response`** → the
WPA2 4-way handshake never starts → the client gives up. `iw dev wlan0 station dump` stays
empty. dmesg (`esp32_spi`) shows the AP iface coming up (`change_iface ... iftype=3`) but no
association/auth events surfaced from firmware.

`did not acknowledge association response` means hostapd/mac80211 transmitted the
**association response** management frame and the driver reported it was **not ACKed** by the
station (via the TX-status path). So either (a) the ESP firmware never actually put that frame
on the air in AP mode, or (b) it transmitted it but never reported the TX-status/ACK back to the
host, so hostapd concludes "not acknowledged" and tears down.

## What is already ruled out (don't re-chase these)

- **Not HT/WMM:** removing `ieee80211n=1` and `wmm_enabled=1` from hostapd — same failure.
- **Not regulatory:** `iw reg set US` + `country_code=US` + `ieee80211d=1` — same failure.
- **Not the config or the client:** the *identical* hostapd.conf and the *same* Galaxy S21
  associate and stream Android Auto fine against a Raspberry Pi's brcmfmac AP. So the host-side
  AP config, DHCP, and the AA app are all correct. The defect is specific to esp-hosted-ng's
  AP-mode frame handling.

## Where to look / likely root cause

esp-hosted-ng is a **fullmac** design (the ESP runs the Wi-Fi MAC). The failing piece is the
**AP-mode management-frame TX path and its TX-status/ACK reporting** between the driver and the
ESP firmware. Investigate, in order:

1. **Is AP mode actually a supported/tested path in this esp-hosted-ng version?** Check the
   driver's advertised interface combinations / `NL80211_IFTYPE_AP` support and the firmware's
   SoftAP support. Confirm whether the intended AP model is "host hostapd drives MLME" vs
   "ESP firmware runs its own SoftAP MLME."
2. **Management-frame TX in AP mode (driver → ESP):** trace how the assoc-response frame handed
   down by hostapd/mac80211 is packetized and sent over SPI to the ESP, and whether the ESP
   firmware transmits host-supplied AP mgmt frames on-air (vs only frames it generates itself
   like beacons/probe responses — which work here).
3. **TX-status / ACK reporting (ESP → driver → mac80211):** verify the firmware reports the
   per-frame TX status (ACK received) back up so `ieee80211_tx_status`/cfg80211 sees the STA
   ACKed. A missing or wrong TX-status is the most common cause of exactly this hostapd message
   even when the frame did reach the client.
4. **Auth vs assoc asymmetry:** auth completes but assoc doesn't — compare how auth frames vs
   assoc-response frames are handled in the firmware AP path (frame size, IEs, the specific
   opcode/queue). The assoc response is larger and carries IEs (HT/rates/WMM); check for a
   truncation, buffer, or frame-type filtering bug in the AP TX path.
5. **Instrument the ESP firmware AP path:** log when an AP mgmt frame is received from the host,
   when it is queued to the PHY, and the hardware TX result/ACK, so you can see whether the
   assoc response is transmitted and whether an ACK is seen.

If host-driven AP MLME is genuinely unsupported by the firmware, the alternative is to make the
ESP run its **native SoftAP** (firmware handles auth/assoc/4-way) configured via the esp-hosted
control interface, and have the host stop running hostapd (host only assigns the IP + runs a
DHCP server). Note that in the head-unit's userland this would also require dropping hostapd —
flag that back to the requester if you conclude host-hostapd AP mode can't be supported.

## Acceptance test (definition of done)

1. With hostapd running the WPA2 AP on `wlan0` (2.4 GHz, ch 6), a client **associates and
   completes the WPA2 4-way handshake**: hostapd log reaches `pairwise key handshake completed`
   / `AP-STA-CONNECTED`, and `iw dev wlan0 station dump` shows the station **authorized**.
2. The client gets a DHCP lease from the head unit's dnsmasq (192.168.53.x on the 192.168.53.1
   AP) and can ping the gateway.
3. End-to-end: the Galaxy S21 joins the `FastCarPlay` AP after the Bluetooth handshake and
   Android Auto video streams (host app already logs `TCP: phone connected` once the phone joins).

## Environment / reference

- Board: Allwinner F1C200s (ARM926), ~36 MB RAM, busybox. Wi-Fi = ESP32 over SPI
  (`esp32_spi`), BT = ESP32 over UART. Controller reports as RivieraWaves. Wi-Fi is 2.4 GHz only.
- AP is driven by `hostapd -dd /tmp/…-hostapd.conf` (`driver=nl80211`). dmesg tag `esp32_spi`.
- Known-good comparison: same hostapd.conf + same phone associates fine on a Pi 5 (brcmfmac).
- Repos: kernel + esp-hosted-ng driver live in `github.com/mbt28/f1c200s-linux`; the ESP32 slave
  firmware is the esp-hosted-ng firmware for the ESP32.
- Useful diagnostics: `hostapd -dd` on wlan0, `dmesg | grep esp32_spi`, `iw dev wlan0 station
  dump`, `iw list` (check supported iftypes / interface combinations), plus firmware-side logging
  of the AP mgmt TX path.
