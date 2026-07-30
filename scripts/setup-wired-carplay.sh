#!/usr/bin/env bash
# Set up wired CarPlay (protocol = carplay-wired) to run WITHOUT sudo.
#
# The app's cp_usbmux needs three things a normal user can't do by default:
#   1. open the iPhone's usbfs node (0x52 reveal + claim the usbmux interface +
#      bulk + USBDEVFS_SETCONFIGURATION to switch to config 6),
#   2. keep the desktop (gphoto2/udisks/ModemManager) from grabbing the iPhone,
#   3. read the MFi auth chip on /dev/i2c-1.
# This installs a udev rule + a module preload + group membership that grant all
# three, so `./out/app settings_cp_wired_pi.txt` runs as your user. Modeled on
# LIVI's installer (99-LIVI.rules + modules-load.d), adapted: we claim the usbfs
# node directly, so we DO grant it (MODE 0660 OWNER); MFi is on i2c-1.
#
# Run once:  ./scripts/setup-wired-carplay.sh    (it re-invokes itself with sudo)
# Then UNPLUG/REPLUG the iPhone (or reboot) so the new rules + modules apply.
set -euo pipefail

TARGET_USER="${SUDO_USER:-$(id -un)}"
UDEV_RULE="/etc/udev/rules.d/99-fastcarplay.rules"
MODULES_CONF="/etc/modules-load.d/fastcarplay.conf"

if [ "$(id -u)" -ne 0 ]; then
    echo "Re-running with sudo (target user: $TARGET_USER)..."
    exec sudo TARGET_USER="$TARGET_USER" bash "$0" "$@"
fi
TARGET_USER="${TARGET_USER:-${SUDO_USER:-root}}"

echo "==> udev rule -> $UDEV_RULE (owner=$TARGET_USER)"
cat > "$UDEV_RULE" <<EOF
# FastCarPlay wired CarPlay (protocol = carplay-wired). Managed by
# scripts/setup-wired-carplay.sh -- do not edit by hand.
#
# Apple (05ac): give this user the usbfs node so cp_usbmux can do the config-6
# reveal + claim the usbmux interface + switch config without root, and stop the
# desktop from auto-mounting the iPhone (PTP/gphoto2 / udisks / ModemManager).
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", MODE="0660", OWNER="$TARGET_USER", ENV{ID_GPHOTO2}="", ENV{GPHOTO2_DRIVER}="none", ENV{ID_MEDIA_PLAYER}="", ENV{UDISKS_IGNORE}="1", ENV{ID_MM_DEVICE_IGNORE}="1"
# The iPhone's USB-NCM link: disable IPv6 duplicate-address-detection so the
# fe80 link-local is usable immediately (CarPlay AV rides it).
ACTION=="add", SUBSYSTEM=="net", ATTRS{idVendor}=="05ac", RUN+="/sbin/sysctl -qw net.ipv6.conf.%k.accept_dad=0"
EOF

echo "==> module preload -> $MODULES_CONF"
cat > "$MODULES_CONF" <<EOF
# FastCarPlay wired CarPlay: bind the iPhone's USB-ethernet (usb0) at cold boot
# so the AV link is ready without a re-plug, and expose /dev/i2c-* for the MFi chip.
cdc_ncm
ipheth
i2c-dev
EOF
modprobe cdc_ncm 2>/dev/null || true
modprobe ipheth 2>/dev/null || true
modprobe i2c-dev 2>/dev/null || true

echo "==> groups: add $TARGET_USER to i2c, plugdev"
for grp in i2c plugdev; do
    getent group "$grp" >/dev/null 2>&1 && usermod -aG "$grp" "$TARGET_USER" || true
done

echo "==> reload udev"
udevadm control --reload-rules
udevadm trigger

# Switching the iPhone to config 6 writes the root-owned sysfs bConfigurationValue
# (the kernel force-detaches ipheth/cdc_ncm/usbmuxd -- the USBDEVFS_SETCONFIGURATION
# ioctl EBUSYs while any driver holds an interface). Grant the app just that one
# privilege via a file capability so it needs no sudo. NOTE: re-run this (or
# `sudo setcap cap_dac_override+ep out/app`) after every rebuild -- linking makes
# a fresh binary without the capability.
APP="$(cd "$(dirname "$0")/.." && pwd)/out/app"
if [ -x "$APP" ]; then
    echo "==> setcap cap_dac_override+ep $APP"
    setcap cap_dac_override+ep "$APP"
else
    echo "==> app not built yet -- after 'make USE_CP_WIRED=1', run: sudo setcap cap_dac_override+ep out/app"
fi

cat <<EOF

Done. Next:
  1. UNPLUG and REPLUG the iPhone (so the udev rule + module preload apply), or reboot.
  2. Make sure the MFi chip is powered (GPIO4 high on this board):
        pinctrl set 4 op dh
     For boot persistence add 'gpio=4=op,dh' to /boot/firmware/config.txt.
  3. Log out/in (or reboot) if the i2c/plugdev group membership is new (id shows the groups).
  4. Run as your user (NO sudo):  ./out/app settings_cp_wired_pi.txt
  Re-run 'sudo setcap cap_dac_override+ep out/app' after each rebuild.
EOF
