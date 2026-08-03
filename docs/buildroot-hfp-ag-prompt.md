# Task: add a working HFP Audio Gateway to the F1C200s image (for wireless Android Auto)

You are working in the **f1c200s-linux** buildroot tree (github.com/mbt28/f1c200s-linux)
that builds the SD-card image for an Allwinner **F1C200s** board (ARM926, ~36 MB RAM free,
busybox userland, bluez 5.79, ESP32 esp-hosted Wi-Fi+BT). The image runs the `fastcarplay`
app which implements **wireless Android Auto** (BT bootstrap + Wi-Fi AP + TCP). The app side
is finished and correct — do NOT try to change app behavior.

## The problem (root cause, already diagnosed)

Wireless Android Auto **fails on the board but works on a reference Raspberry Pi 5** running
the *identical* app binary. HCI traces (btmon) show:

- BT pairing/bonding succeeds on both (DisplayYesNo / Numeric Comparison).
- SDP is correct on both — the board serves the AA record (UUID
  `4de17a00-52cb-11e6-bdf4-0800200c9a66`, RFCOMM ch 8).
- **The difference:** the phone will only *start* Android Auto with a head unit that also
  presents a **working HFP Audio Gateway (HFP-AG, UUID `0x111f`)**. On the Pi the phone opens
  a second RFCOMM channel and runs the HFP Service Level Connection; the AG answers it:

  ```
  phone → AT+BRSF=695        AG → +BRSF: 4079 / OK
  phone → AT+BAC=1,2,3       AG → OK
  phone → AT+CIND=?          AG → +CIND: ("call",(0,1)),... / OK
  phone → AT+CIND?           AG → +CIND: 0,0,0,0,... / OK
  phone → AT+CMER=3,0,0,1    AG → OK
  phone → AT+CHLD=?          AG → OK
  ```

  The board has **no HFP-AG**, so the phone never proceeds to the AA channel and disconnects.

**Critical fact:** *bluez alone does NOT answer this AT handshake.* On the reference Pi the AG
is provided by **PipeWire's bluez5 SPA plugin with its native HFP backend** (`libspa-bluez5.so`;
PipeWire + pipewire-pulse + WirePlumber are running; **no oFono**). bluez just registers the
profile that the backend provides. So the image needs a **userspace HFP-AG backend**, not just
bluez.

Also observed on the Pi (all a consequence of the HFP-AG being registered):
- Class of Device becomes `0x600000` (Audio + **Telephony** service bits). Board is `0x000000`.
- `sdptool browse local` shows `Handsfree Audio Gateway (0x111f)` and `Handsfree (0x111e)`.

## Goal

Ship, in the F1C200s image, a component that registers an **HFP Audio Gateway** with bluez and
answers the SLC AT handshake **without needing a real modem/cellular** (canned responses are
fine — this is a car head unit, not a phone). It must run headless from the board's init.
No calls/audio routing are required for AA to start — only that the SLC completes.

## Candidate solutions — pick the lightest that actually provides the AG role

The board is RAM-constrained (~36 MB free), so footprint matters. Evaluate against the packages
available in this buildroot tree:

1. **bluez-alsa (bluealsad) — try first, lightest.** `BR2_PACKAGE_BLUEZ_ALSA`. Recent bluez-alsa
   has a **native HFP backend** (no oFono) and can run the AG profile:
   `bluealsad -p hfp-ag` (plus `-p a2dp-source`/etc. if wanted). Confirm the buildroot version
   is new enough to have the built-in HFP backend (older versions required oFono). Verify its
   HFP build option is enabled and add an init script to launch `bluealsad -p hfp-ag` after
   `bluetoothd`. This is the smallest addition.

2. **PipeWire + WirePlumber + spa bluez5 (matches the Pi exactly).**
   `BR2_PACKAGE_PIPEWIRE` (with bluez5/spa bluez support) + `BR2_PACKAGE_WIREPLUMBER`.
   The native HFP backend is what the Pi uses. Heavier; only if bluez-alsa can't do AG.

3. **oFono** (`BR2_PACKAGE_OFONO`) — traditional bluez HFP companion, but it is telephony/modem
   oriented and awkward/uncertain for the pure-AG head-unit role. Lowest preference; use only if
   1 and 2 don't pan out.

Whichever you choose: make sure **bluez** is built with the profile support the backend expects,
and that the backend daemon is **started at boot after bluetoothd**. The head unit's CoD should
then automatically gain the Audio+Telephony bits (like the Pi's `0x600000`) once the AG profile
is registered — no manual `hciconfig class` needed.

## Acceptance test (definition of done)

On the running board, with `dbus-daemon`, `bluetoothd`, and the new HFP backend all up:

1. `bluetoothd` started with `--compat` (creates `/var/run/sdp`), then
   `sdptool browse local` shows a **`Handsfree Audio Gateway` (0x111f)** record.
2. `hciconfig hci0 class` shows **Audio + Telephony** service classes (non-zero, ~`0x6…`).
3. Pair a phone; a `btmon -w` capture shows the phone opening the **HFP RFCOMM channel** and the
   **`AT+BRSF` / `AT+CIND` SLC being answered** with `+BRSF:`/`+CIND:`/`OK` (as in the transcript
   above).
4. **End-to-end:** the phone starts **Android Auto** (the app then logs `bt: NewConnection /fcp/aa`
   → aaw handshake → Wi-Fi join → TCP → video). This is the real success signal.

## Reference facts / constraints

- Board bluez: **5.79**. Kernel already has `CONFIG_BT_RFCOMM` (RFCOMM works — verified).
- The app already: registers the pairing agent as `DisplayYesNo`, advertises the AA SDP record,
  and registers a passive HFP *client* on `/fcp/hfp` (that client does nothing useful and can be
  ignored — it is not the AG the phone needs).
- Deploy/verify on the board is over serial + a USB-eth adapter (no Wi-Fi for management). busybox
  has **no `pkill`/`pgrep`** (use `pidof`+`kill`); **scp/sftp is broken** on the image
  (`ssh host 'cat > file' < local`).
- Keep the image small; prefer the minimal daemon set. Don't pull in a display/audio server stack
  you don't need just to get the AG.

## Fallback (if the image route proves too heavy/uncertain)

A minimal HFP-AG can instead be implemented inside the `fastcarplay` app itself (register a
`0x111f` server profile via bluez's Profile1 API and answer the SLC AT commands with canned
replies on the RFCOMM fd). Mention this back to the requester if none of the image options are
viable within the RAM budget — but the app-side change is out of scope for you.
