#!/bin/bash
#
# teardown_gadget.sh  –  Unbind gadget and stop daemon
#
# Called by: ipp-printer.service ExecStop
#

GADGET=/sys/kernel/config/usb_gadget/ipp_printer
FFSDIR=/dev/ffs-ipp0

# ── Stop daemon ───────────────────────────────────────────────────────────────
if [ -f /run/ipp-printer.pid ]; then
    PID=$(cat /run/ipp-printer.pid)
    kill "$PID" 2>/dev/null || true
    rm -f /run/ipp-printer.pid
fi

sleep 1

# ── Unbind UDC ────────────────────────────────────────────────────────────────
if [ -f "$GADGET/UDC" ]; then
    echo "" > "$GADGET/UDC" 2>/dev/null || true
fi

# ── Clean up os_desc symlink if it was ever created ──────────────────────────
[ -L "$GADGET/os_desc/c.1" ] && rm -f "$GADGET/os_desc/c.1"
[ -f "$GADGET/os_desc/use" ] && echo 0 > "$GADGET/os_desc/use"

# ── Remove function symlink and unmount FunctionFS ────────────────────────────
[ -L "$GADGET/configs/c.1/ffs.ipp0" ] && rm -f "$GADGET/configs/c.1/ffs.ipp0"
mountpoint -q "$FFSDIR" && umount "$FFSDIR" 2>/dev/null || true

# ── Remove gadget configfs tree ───────────────────────────────────────────────
rmdir "$GADGET/configs/c.1/strings/0x409" 2>/dev/null || true
rmdir "$GADGET/configs/c.1"               2>/dev/null || true
rmdir "$GADGET/functions/ffs.ipp0"        2>/dev/null || true
rmdir "$GADGET/strings/0x409"             2>/dev/null || true
rmdir "$GADGET"                           2>/dev/null || true

echo "Gadget removed."
