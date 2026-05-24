#!/bin/bash
# gadget_enable.sh – Bind the gadget to the UDC (USB Device Controller).
#
# Must be called AFTER the usb_gadget daemon has written its descriptors to
# ep0 (i.e. after it prints "descriptors written").

set -euo pipefail

GADGET="/sys/kernel/config/usb_gadget/g-loopback"

# Auto-detect the first available UDC.
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "${UDC}" ]; then
    echo "gadget_enable: no UDC found – is dwc2 loaded in peripheral mode?" >&2
    exit 1
fi

echo "${UDC}" > "${GADGET}/UDC"
echo "gadget_enable: bound to UDC '${UDC}'"
