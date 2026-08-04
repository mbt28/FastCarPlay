# Roadmap: native wired + wireless CarPlay for FastCarPlay (low-end first)

**Vision.** FastCarPlay OS as the **open, maintained replacement firmware** for head units that
already ship an **MFi authentication coprocessor + license** but run outdated, closed-source
software. The owner supplies the MFi chip + license; FastCarPlay supplies up-to-date, free,
maintained CarPlay/AA. **The F1C200s (408 MHz single-core ARM926, no NEON/FPU-heavy, ~36 MB free
RAM, cedrus HW H.264) is the low-end reference target — everything is designed to run well there
first, so it runs everywhere.**

Reference blueprint: **LIVI PR #303** (`444d8c2`, wired+wireless CarPlay, multi-session). LIVI is
a desktop TS/Python impl; **treat its specifics as leads to verify against source**, and expect to
re-implement its Python (usbmux/iAP2/mDNS/MFi) as lean C++ — Python does not ship on the board.

---

## 1. MFi = a hardware-abstraction driver (not a gate)

The chip is assumed present; the license is the owner's responsibility. So the work is a small
**`MfiAuth` driver**: probe the coprocessor over **I²C** (support the common variants — MFi 2.0C
/ 3.0, addresses ~0x10/0x11, differing register maps), read its certificate, and run
challenge→signature. Abstract behind one interface so different boards/chips drop in. Kernel: an
i2c bus (`i2c-mv64xxx`/`i2c-gpio`) + our userspace driver (no out-of-tree kernel module needed —
plain `/dev/i2c-N`). This is the only genuinely CarPlay-specific *hardware* dependency.

## 2. Design principles for low-end (the efficiency mandate)

These are requirements, not nice-to-haves — they shaped the phase ordering:

1. **Zero-copy AV, always.** Video stays as cedrus `DRM_PRIME` dma-buf → DEFE (reuse the
   framebuffer-cache win, commit `ec9b468`). Never a per-frame memcpy or SW colour convert.
2. **Cheap per-frame crypto.** CarPlay session encryption is per-message AEAD — use
   **ChaCha20-Poly1305** (we measured it ~4× cheaper than AES-GCM on this ARM926; TLS thread went
   39%→10%). The expensive part (**SRP-6a pair-setup, 2048-bit modexp**) is **one-time per phone**
   and cached — never on the hot path.
3. **One crypto dependency.** OpenSSL 3.x (already linked) covers X25519, HKDF, ChaCha20-Poly1305,
   and BN for SRP. **Avoid libsodium/mbedtls** unless a gap forces it — no new dep, smaller flash.
4. **Lean everything else.** A **minimal mDNS responder** (announce one service + answer, ~a few
   hundred LOC) instead of Avahi; a **trimmed usbmux/iAP2** (just attach + connect-to-service +
   tunnel) instead of libimobiledevice; **no Python**.
5. **Static/pooled buffers.** Reuse `UsbBuffer` slot pools + `Message::Payload` allocate-then-fill
   (one copy per frame max). No per-frame heap churn. Mind the **16 MB CMA** ceiling shared with
   the VPU (zero-copy USB `dev_mem_alloc` is banned — starves cedrus; kernel MUSB-DMA is the way).
6. **Single-core-aware threading.** Mirror the proven AA model: `read` (RT prio) / `process` /
   `write`, one AEAD mutex, no thread explosion. Multi-session deferred.
7. **Negotiate the cheapest media.** Advertise **H.264-only at panel resolution (800×480@30)** —
   skip H.265/VP9/AV1 negotiation; minimise bitrate → less decrypt + decode + USB.
8. **Build-gated (`USE_CARPLAY`).** Boards that don't need CarPlay don't pay the flash/RAM for it.
9. **Consolidate, don't duplicate.** Promote the AA-specific transports to a **shared core**
   (`UsbTransport`, `TcpTransport`, `WifiAp`, `BtBootstrap`) used by both AA and CP — one code
   path, less flash/RAM, one place to optimise.

## 3. Reuse the existing seam

`IConnection` + `application.cpp:423 makeConnection()` (switch on `protocol`) + the AA transport
abstraction + protocol-agnostic `cedrus_decoder`/`drm_display`/`pcm_audio`/`touch_input`/`message.h`
already exist. A **`CpConnection : IConnection`** emits the **same Carlinkit-shaped Messages** →
decode/display/audio/input reused **unchanged** (exactly how `AaConnection` did it). Add
`protocol = cp-usb | cp-wireless`.

## 4. Phased roadmap (ordered by risk × reuse, efficiency baked in)

### P0 — `MfiAuth` I²C driver
Probe/identify the coprocessor, read cert, challenge→signature. **Milestone:** valid signature for
a test challenge on the board; abstraction covers ≥1 chip variant. *(Blocking for any real iPhone.)*

### P1 — Pairing + session-crypto substrate (OpenSSL-only, **desktop, real iPhone**)
pair-setup (SRP-6a) + pair-verify (X25519 + HKDF-SHA512) + per-message ChaCha20-Poly1305; pairing
persistence; MFi challenge folded into the handshake. `CpConnection` skeleton. **Milestone:**
handshake completes with a real iPhone, encrypted control channel up, MFi accepted. *(Highest
software uncertainty, no board needed — do it first.)*

### P2 — Shared transport/AV core consolidation
Promote `UsbTransport`/`TcpTransport`/`WifiAp` to shared; define `CpConnection`'s message mapping
(H.264→`CMD_VIDEO_DATA`, AAC→decode→`CMD_AUDIO_DATA`, touch/control→`writeQueue`). Add a lean
**AAC-LC decoder** path (ffmpeg, already linked) into `pcm_audio`. **Milestone:** AA regression
still green after consolidation; AAC decode measured on ARM926 within budget.

### P3 — Wired CarPlay (USB / usbmux / iAP2)
Trimmed C++ usbmux + minimal iAP2 link/control session; tunnel the CarPlay AV protocol; MFi in the
control session. **Milestone (desktop, real iPhone):** CarPlay UI renders + drivable wired, audio
plays, zero-copy video confirmed.

### P4 — Wireless CarPlay (BLE + mDNS + Wi-Fi + TCP)
BLE trigger (extend `aa_bluetooth`) + minimal mDNS responder + reuse the shared `wifi_ap` AP + `TcpTransport`.
**Milestone (desktop):** iPhone joins via Bonjour and runs CarPlay wirelessly.

### P5 — F1C200s efficiency bring-up + perf tuning
Cross-compile (`USE_CARPLAY` + cedrus); presets; end-to-end video/touch/audio on the board;
**profile per-thread CPU + RAM** (same method as the AA work) and tune to the budgets in §6; wire
MFi I²C into buildroot. **Milestone:** CarPlay on the board, wired then wireless, inside budget.

### P6 — Testing (unit + integration + regression) — continuous from P1
- **Unit** (`examples/` harness pattern, like `aa_aaw_test`): SRP/X25519/HKDF/ChaCha round-trips
  and known-answer vectors; TLV/frame encode↔decode fuzz; control-message parsers; MFi challenge
  against a mock; pairing persistence.
- **Integration:** real iPhone, wired + wireless, across **iOS versions and phone models**; Siri,
  phone calls, nav, now-playing, night mode, touch accuracy.
- **Regression:** replay captured sessions; **usbmon/pcap diff vs a LIVI session**; ASAN on the
  desktop build through P1–P4; CI runs the unit suite per commit.
- **On-device:** F1C200s video/touch/audio checklist; the `transfered()`/fps/CPU debug overlay.

### P7 — Stress + soak testing (the "does it survive a car" phase)
- **Connect storms:** rapid plug/unplug (wired) and Wi-Fi/BLE reconnect storms (wireless); phone
  reboot mid-session; lock/unlock; incoming-call + Siri interrupts; AOAP/usbmux re-enum races.
- **Soak:** 24 h+ continuous + hourly reconnects; **RAM plateau / leak check**; sustained-CPU +
  thermal; frame-count/latency drift.
- **Degraded conditions:** weak RSSI, 2.4 GHz congestion, channel hop; brownout/weak-5 V USB;
  MFi-chip timeout/failure handling; **frame-desync/corruption injection** → must recover, never
  wedge (the AA "desync is fatal → hard reconnect" discipline; note the *wired-AA MUSB-DMA
  BUS_SEL* corruption we hit — CarPlay's TLS-equiv MAC check will surface the same, so the
  transport must be integrity-clean).
- **Multi-session (if built):** wired + wireless (or two phones) concurrent; focus-switch storms;
  plane-arbitration (`bound-tag`) races.
- **Acceptance:** no leaks, no lockups, bounded reconnect, graceful degradation, no corruption
  reaching the decoder.

### P8 — Multi-session + polish (optional, matches LIVI)
`CpManager` owning multiple `CpSession`s, focus/suspend, compositor plane arbitration. On the
F1C200s a **single active session** is the sane default; multi-session is a later nicety and a RAM
cost to justify.

## 5. Efficiency budgets (measure against these on the F1C200s)
- Video path: **zero per-frame memcpy**, decoder+display thread ≤ ~20% core (post FB-cache).
- Session crypto (per-frame): ChaCha20-Poly1305 ≤ ~10–15% core at 800×480@30.
- Pair-setup: one-time, ≤ a couple seconds, off the hot path.
- AAC decode: ≤ ~10% core (measure early — it's the new SW cost vs AA).
- Steady-state RAM: fits with cedrus in 16 MB CMA + ~36 MB system; no growth over a 24 h soak.
- Total wired-CarPlay CPU target: comfortably < 100% with idle headroom (AA hit ~50% after tuning).

## 6. Risks
- **CarPlay is reverse-engineered + Apple can change it** — highest churn risk; track LIVI `dev`.
- **usbmux/iAP2 + mDNS + MFi in lean C++** is the bulk of the new code (no Python crutch on-board).
- **New SW costs on ARM926:** AAC decode + SRP — budget in P2/P5, not at the end.
- **Transport integrity** (MUSB-DMA lesson): any byte corruption → AEAD MAC failure → teardown;
  the kernel USB-DMA path must be clean (see `docs/buildroot-musb-ddma-followup-prompt.md`).

## 7. Decisions to confirm
1. **Wired-first** (mirror the AA rollout, lower risk) — recommended — or wireless-first?
2. **Keep Carlinkit as the CarPlay fallback** while native matures — recommended.
3. Vendor a trimmed usbmux/pairing C lib vs. port LIVI's logic to C++ from scratch (portability +
   flash size tradeoff).
4. Which MFi coprocessor variant(s) to support first (drives the P0 driver).

**Recommended first move:** P1 (pairing crypto, OpenSSL-only) on desktop against a real iPhone,
in parallel with P0 (`MfiAuth` I²C driver) — those two unblock everything and carry the most risk.
