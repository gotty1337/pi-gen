#!/bin/bash
# gadget_teardown.sh – Detach UDC and remove configfs gadget entries.

set -euo pipefail

GADGET_NAME="g-printer"
GADGET="/sys/kernel/config/usb_gadget/${GADGET_NAME}"
FUNCTION="printer.0"

[ -d "${GADGET}" ] || exit 0

echo "" > "${GADGET}/UDC" 2>/dev/null || true
rm -f "${GADGET}/configs/c.1/${FUNCTION}"
rmdir "${GADGET}/configs/c.1/strings/0x409" 2>/dev/null || true
rmdir "${GADGET}/configs/c.1"               2>/dev/null || true
rmdir "${GADGET}/strings/0x409"             2>/dev/null || true
rmdir "${GADGET}/functions/${FUNCTION}"     2>/dev/null || true
rmdir "${GADGET}"                           2>/dev/null || true

echo "gadget_teardown: done"
