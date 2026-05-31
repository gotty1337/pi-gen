#!/bin/bash
# gadget_setup.sh – Configure a USB FunctionFS bulk-loopback gadget via configfs.
#
# Idempotent: re-running while the gadget is already configured is a no-op.

set -euo pipefail

GADGET_NAME="g-loopback"
CONFIGFS="/sys/kernel/config"
GADGET="${CONFIGFS}/usb_gadget/${GADGET_NAME}"
FFS_DIR="/dev/ffs-loopback"

modprobe libcomposite

if ! mountpoint -q "${CONFIGFS}"; then
    mount -t configfs none "${CONFIGFS}"
fi

if [ -d "${GADGET}" ]; then
    echo "gadget_setup: ${GADGET_NAME} already exists, skipping"
    exit 0
fi

mkdir -p "${GADGET}"
echo 0x1d6b > "${GADGET}/idVendor"   # Linux Foundation
echo 0x0104 > "${GADGET}/idProduct"  # Multifunction Composite Gadget
echo 0x0200 > "${GADGET}/bcdUSB"     # USB 2.0
echo "full-speed" > "${GADGET}/max_speed"   # FS only – no HS descriptors needed
echo 0x00   > "${GADGET}/bDeviceClass"    # Class defined at interface level
echo 0x00   > "${GADGET}/bDeviceSubClass"
echo 0x00   > "${GADGET}/bDeviceProtocol"

# Use the SoC CPU serial as the USB serial number so each device is unique.
# Falls back to a fixed string if the serial is unavailable.
SERIAL=$(awk '/^Serial/ {print $3}' /proc/cpuinfo 2>/dev/null | tr -d '[:space:]')
SERIAL="${SERIAL:-deadbeef00000000}"

mkdir -p "${GADGET}/strings/0x409"
echo "${SERIAL}"         > "${GADGET}/strings/0x409/serialnumber"
echo "Raspberry Pi"      > "${GADGET}/strings/0x409/manufacturer"
echo "USB IPP Printer" > "${GADGET}/strings/0x409/product"

mkdir -p "${GADGET}/configs/c.1"
echo 250 > "${GADGET}/configs/c.1/MaxPower"
mkdir -p "${GADGET}/configs/c.1/strings/0x409"
echo "IPP Printer config" > "${GADGET}/configs/c.1/strings/0x409/configuration"

mkdir -p "${GADGET}/functions/ffs.usb0"

mkdir -p "${FFS_DIR}"
if ! mountpoint -q "${FFS_DIR}"; then
    mount -t functionfs usb0 "${FFS_DIR}"
fi

ln -sf "${GADGET}/functions/ffs.usb0" "${GADGET}/configs/c.1/"

echo "gadget_setup: done"
