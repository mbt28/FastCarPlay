# Task: MUSB DDMA follow-ups for wired Android Auto (f1c200s-linux)

You work in the **f1c200s-linux** buildroot/kernel tree (`~/projects/f1c200s/f1c200s-linux/`)
for an Allwinner **F1C200s** wired Android Auto head unit. Kernel 6.6.143. The MUSB **DDMA**
support (patches `0016-suniv-musb-dma-via-dedicated-dma-engine` + `0017-lctech-f1c200s-wire-musb-ddma`,
`CONFIG_USB_SUNXI_DMA=y`, DT `dmas = <&dma SUN4I_DMA_DEDICATED 4>`) is in and **working**: wired-AA
H.264 bulk-IN is DMA'd (`prog ep2 rx len=512 … -> DMA` / `DMA rx done ep2 actual_len=512`), which
is the intended ARM926 CPU offload.

**Validated on hardware (2026-07-13):** with the app-side control-write retry (FastCarPlay
`f1c200s-cedrus`, "retry control-channel USB writes on clean transient timeout"), wired AA now
sustains **3+ minutes of continuous 30 fps video, no reconnect loop**; the retry recovered **8**
BUS_SEL/DMA collisions (`Bulk write timeout (10 bytes), retry 1/6`) with zero session teardowns.
The per-transfer console-print flood you already fixed is confirmed gone (0 musb lines at
console loglevel 7 during video). So the datapath is good. Two kernel follow-ups remain, plus
shipping the app fix.

## 1. Kernel bug: `musb_h_tx_flush_fifo` WARNING on URB cancel (fix this)

Every time libusb cancels a timed-out TX URB (which the write-retry now does on each BUS_SEL
collision), the host TX-FIFO flush fails:

```
WARNING: CPU: 0 PID: 262 at drivers/usb/musb/musb_host.c:113 musb_h_tx_flush_fifo+0xdc/0x104
musb-hdrc musb-hdrc.1.auto: Could not flush host TX2 fifo: csr: 2003
  musb_h_tx_flush_fifo <- musb_cleanup_urb <- musb_urb_dequeue <- usb_hcd_unlink_urb
  <- usb_kill_urb <- usbdev_ioctl        (Comm: aa-read)
```

Not fatal now (console no longer floods, so no RT throttling), but it fires on **every** retry's
cancel and each one dumps a full stack trace (CPU cost) — it should be eliminated.

**Root cause hypothesis:** `musb_cleanup_urb()` calls `musb_h_tx_flush_fifo()`, which spins on
the TXCSR waiting for `FIFONOTEMPTY|TXPKTRDY` (CSR `0x2003`) to clear — but the endpoint was left
in **DDMA / BUS_SEL=1** mode, so the CPU/PIO view of the FIFO never drains and the flush times
out. The DDMA teardown path isn't quiescing the DMA + restoring PIO before the core flushes.

**Fix direction:** in the sunxi DDMA driver's **`channel_abort`** (and the dequeue path), before
`musb_h_tx_flush_fifo` runs: abort/terminate the sun4i-dma slave channel for that endpoint, clear
`USB_EFR.BUS_SEL` back to 0 (PIO) for the EP, and clear the DMA-mode TXCSR bits (`DMAENAB`/
`DMAMODE`) so the core can flush the FIFO normally. Mirror how `musb_cppi41.c` / `ux500_dma.c`
handle `channel_abort` + the musb_host cleanup ordering. Verify `musb->dma_controller->channel_abort`
is actually invoked on `musb_cleanup_urb` for a DMA-active TX EP.

## 2. Confirm the per-transfer debug prints are *gated*, not just quieted

You fixed the flood — confirm the `prog ep… -> DMA/PIO(reject)`, `VEND0=…`, `DMA rx done` prints
are behind **`dev_dbg`** (dynamic-debug, off by default, re-enablable via
`/sys/kernel/debug/dynamic_debug/control`), not deleted and not `dev_info`. They're invaluable for
future bring-up; they just must never be on the hot path by default.

## 3. App fix ships automatically — no action needed

The FastCarPlay buildroot package auto-tracks the **`f1c200s-cedrus`** branch tip and
`settings_cedrus_aa.txt` ships from that repo, so the control-write retry commit (`8ac0582`) and
its `usb-write-*` settings come in on the next image build. Nothing to do here — listed only so
you know the app side is already covered. This kernel task is **only items 1 and 2 above.**

## Acceptance
1. Wired AA sustains video (already validated) — regression check it still holds.
2. **No `musb_h_tx_flush_fifo` / "Could not flush host TX2 fifo" WARNING** in `dmesg` across a
   session with retries firing (grep dmesg after ~2 min of AA).
3. Per-transfer musb prints **off by default**, re-enablable via dynamic-debug.
4. `use_dma=1`, DDMA active (`prog … -> DMA` on RX), CPU during wired AA lower than the PIO
   baseline (the whole point of the DDMA work).

## Reference
- App fix + full profiling context: FastCarPlay `docs/` (wired-AA optimization) and the commit
  on `f1c200s-cedrus`.
- Study doc already in-tree: `docs/musb-dma-study.md` (short-packet / BUS_SEL mutual-exclusion /
  DRQ mux — the same mechanics that make the TX-cancel flush tricky).
- Escape hatch remains `use_dma=0` (module param) → PIO fallback.
