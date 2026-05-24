#!/bin/bash
# gadget_teardown.sh – Detach UDC and remove configfs gadget entries.

set -euo pipefail

GADGET="/sys/kernel/config/usb_gadget/g-loopback"
FFS_DIR="/dev/ffs-loopback"

[ -d "${GADGET}" ] || exit 0

echo "" > "${GADGET}/UDC" 2>/dev/null || true
rm -f "${GADGET}/configs/c.1/ffs.usb0"
rmdir "${GADGET}/configs/c.1/strings/0x409" 2>/dev/null || true
rmdir "${GADGET}/configs/c.1"               2>/dev/null || true
rmdir "${GADGET}/strings/0x409"             2>/dev/null || true
rmdir "${GADGET}/functions/ffs.usb0"        2>/dev/null || true
rmdir "${GADGET}"                           2>/dev/null || true

if mountpoint -q "${FFS_DIR}"; then
    umount "${FFS_DIR}"
fi

echo "gadget_teardown: done"
