#!/bin/bash
# gadget_enable.sh – Bind the gadget to the UDC after descriptors are written.
#
# Polls for ep1 inside the FFS mount, which the kernel creates only after
# the daemon successfully writes its descriptors to ep0.

set -euo pipefail

GADGET="/sys/kernel/config/usb_gadget/g-loopback"
FFS_DIR="/dev/ffs-loopback"
TIMEOUT=15

echo "gadget_enable: waiting for descriptors..."
ELAPSED=0
while [ ! -e "${FFS_DIR}/ep1" ]; do
    sleep 0.1
    ELAPSED=$(awk "BEGIN{print ${ELAPSED}+0.1}")
    if awk "BEGIN{exit !(${ELAPSED} >= ${TIMEOUT})}"; then
        echo "gadget_enable: timeout waiting for ep1" >&2
        exit 1
    fi
done

UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "${UDC}" ]; then
    echo "gadget_enable: no UDC found" >&2
    exit 1
fi

echo "${UDC}" > "${GADGET}/UDC"
echo "gadget_enable: bound to UDC '${UDC}'"
