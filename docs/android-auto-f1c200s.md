# Native wired Android Auto on the F1C200s — deployment & bring-up

This is the checklist for running the dongle-free Android Auto backend
(`protocol = aa-usb`) on the F1C200s. The protocol is fully verified on desktop
(Raspberry Pi 5); this covers getting it onto the board and the on-device
bring-up. Profile file: `settings_cedrus_aa.txt`.

## 0. Prerequisites

- The phone must plug into the **board's USB host port** (the single OTG port in
  host mode). The serial console (CH340 on the dev machine's `/dev/ttyUSB0`) is a
  separate UART, so it stays free for watching logs.
- The board has **no network and ~36 MB free RAM**. Deploy over **SD card**, not
  the serial console (a raw 115200 paste corrupts bulk data).
- A USB **data** cable to the phone (charge-only cables won't enumerate).

## 1. Cross-build (on the dev machine)

```sh
BR=/home/mtekbas/projects/f1c200s/kernel7.1.2/f1c200s-linux/output/host
cd FastCarPlay
make clean
make USE_CEDRUS=1 \
  CXX=$BR/bin/arm-buildroot-linux-gnueabi-g++ \
  CC=$BR/bin/arm-buildroot-linux-gnueabi-gcc  \
  PKG_CONFIG=$BR/bin/pkg-config
$BR/bin/arm-buildroot-linux-gnueabi-strip -o out/app-f1c200s out/app
```

Verify: `readelf -A out/app | grep CPU_arch` → `Tag_CPU_arch: v5TEJ`.
(A prebuilt `out/app-f1c200s`, 1.1 MB stripped, is already staged.)

## 2. Deploy via SD card

1. Power off the board, pull its SD card, mount its rootfs on the dev machine.
2. Copy the binary next to your existing install (replace `<APP_DIR>` with wherever
   the current FastCarPlay `app` lives on the board — e.g. `/root` or `/opt/fastcarplay`):
   ```sh
   sudo cp out/app-f1c200s        <SD_ROOTFS>/<APP_DIR>/app-aa
   sudo cp settings_cedrus_aa.txt <SD_ROOTFS>/<APP_DIR>/
   ```
   Keep it as a **separate** `app-aa` binary + settings so the working Carlinkit
   setup is untouched and you can fall back instantly.
3. Unmount, reinsert, power on.

## 3. First run (over the serial console)

At the board's root shell (`#`), stop any autostarted Carlinkit instance first
(so it isn't holding the USB port), then:

```sh
cd <APP_DIR>
./app-aa settings_cedrus_aa.txt
```

Plug the phone in, unlock it, and **accept the "allow Android Auto / this
accessory" prompt** on the phone (first connection only). Watch the console for
this sequence:

```
[aoap] AOAP capable device (protocol 2), switching to accessory mode
[AaConnection] Accessory linked ...
[AaConnection] Version handshake done (1.7), starting TLS
[AaConnection] TLS established (ECDHE-RSA-AES128-GCM-SHA256)
[AaConnection] Service discovery request from '<your phone>'
[AaConnection] Channel N open request ...   (x6: sensor/video/audio/mic/input)
[AaConnection] Media start on channel 3 ...
[AaConnection] Phone connected (Android Auto)
```

At that point the **Android Auto screen should appear on the board's panel**
(cedrus decode → DEFE scale to 480×272).

## 4. What to verify on the device (needs eyes/hands on the board)

- [ ] Video renders on the panel and is smooth (not stuttering/tearing).
- [ ] Touch works — tap an app icon via the GT911; it should respond.
- [ ] (If `audio-driver` set to a real driver) media audio plays.

## 5. The key technical question — does the arm926 keep up?

The open risk is whether the ~500 MHz ARM926 can sustain **TLS-decrypt + framing
+ feeding the H.264 decoder** at video bitrate. Judge it from the serial console
without needing to see the panel:

- Set `debug-overlay = true` in the profile, or run a second serial session and
  watch `top` while connected. Look at CPU% of `app-aa` and the `usb-read` thread.
- Frame health: with `log-level = 4`, count `RX ch 3 id 0x0000` lines over 10 s —
  a healthy 30 fps stream is ~300 in 10 s. A number well below that (with rising
  `videoStream` queue depth in the status line) means the CPU can't keep up.
- Benchmark the raw crypto cost once on the board:
  `openssl speed -evp aes-128-gcm` — the AA video path needs only ~1 MB/s, so a
  few MB/s here is enough headroom.

If it can't keep up: the mitigations, in order — confirm `aa-video-fps = 30` and
`aa-resolution = 1` (already the lightest), raise `async-usb-calls`, and check the
cedrus decoder is actually engaged (not silently falling back to software — the
log prints `SW decoder h264` if it does).

## Troubleshooting

| Symptom (serial log) | Likely cause / fix |
|---|---|
| Loops on `AOAP probing` forever | Phone not AOAP-capable via this cable (charge-only?) or `aa-vendor-id`/`aa-product-id` pinned wrong. Auto-scan (0/0) is default. |
| `Accessory linked` then repeats, `ERROR_TIMEOUT` on first write | Wedged accessory session — the code resets the device to re-switch; if it persists, unplug/replug the phone. |
| Reaches `TLS established` then phone drops before discovery | The shared HU certificate was rejected (Google revocation) — see `aa_cert.h`. No workaround. |
| `Phone connected` but panel is black | cedrus/DEFE display issue, not protocol — same path as the Carlinkit cedrus profile; check `renderer = drm`. |
| Video stutters, `videoStream` queue grows | arm926 can't keep up — see section 5. |
| Permission/claim error opening USB | Running as non-root without the udev rule; on the board you're root, so N/A. |
