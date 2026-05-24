#!/bin/bash
# gadget_teardown.sh – Cleanly remove the FunctionFS loopback gadget.
#
# Called automatically by the usb-gadget.service ExecStopPost.

set -euo pipefail

GADGET="/sys/kernel/config/usb_gadget/g-loopback"
FFS_DIR="/dev/ffs-loopback"

[ -d "${GADGET}" ] || exit 0

# Detach from UDC first.
echo "" > "${GADGET}/UDC" 2>/dev/null || true

# Remove function symlink.
rm -f "${GADGET}/configs/c.1/ffs.usb0"

# Remove config strings / config.
rmdir "${GADGET}/configs/c.1/strings/0x409" 2>/dev/null || true
rmdir "${GADGET}/configs/c.1"               2>/dev/null || true

# Remove gadget strings / function / gadget.
rmdir "${GADGET}/strings/0x409"         2>/dev/null || true
rmdir "${GADGET}/functions/ffs.usb0"    2>/dev/null || true
rmdir "${GADGET}"                       2>/dev/null || true

# Unmount FunctionFS.
if mountpoint -q "${FFS_DIR}"; then
    umount "${FFS_DIR}"
fi

echo "gadget_teardown: done"
