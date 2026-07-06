#!/bin/bash
# gadget_setup.sh – Configure a raw USB Printer class gadget via configfs.
#
# Uses the kernel's built-in "printer" configfs function (usb_f_printer),
# which exposes the received print data directly at /dev/usb/lp0. No
# userspace protocol translation is involved – whatever the host writes
# to the printer port shows up as raw bytes on that device node.
#
# Idempotent: re-running while the gadget is already configured is a no-op.

set -euo pipefail

GADGET_NAME="g-printer"
CONFIGFS="/sys/kernel/config"
GADGET="${CONFIGFS}/usb_gadget/${GADGET_NAME}"
FUNCTION="printer.0"

modprobe libcomposite
modprobe usb_f_printer 2>/dev/null || true

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
echo 0x00   > "${GADGET}/bDeviceClass"    # Class defined at interface level
echo 0x00   > "${GADGET}/bDeviceSubClass"
echo 0x00   > "${GADGET}/bDeviceProtocol"

# Use the SoC CPU serial as the USB serial number so each device is unique.
# Falls back to a fixed string if the serial is unavailable.
SERIAL=$(awk '/^Serial/ {print $3}' /proc/cpuinfo 2>/dev/null | tr -d '[:space:]')
SERIAL="${SERIAL:-deadbeef00000000}"

mkdir -p "${GADGET}/strings/0x409"
echo "${SERIAL}"    > "${GADGET}/strings/0x409/serialnumber"
echo "Raspberry Pi" > "${GADGET}/strings/0x409/manufacturer"
echo "USB Printer"  > "${GADGET}/strings/0x409/product"

mkdir -p "${GADGET}/configs/c.1"
echo 250 > "${GADGET}/configs/c.1/MaxPower"
mkdir -p "${GADGET}/configs/c.1/strings/0x409"
echo "Printer config" > "${GADGET}/configs/c.1/strings/0x409/configuration"

mkdir -p "${GADGET}/functions/${FUNCTION}"

# IEEE 1284 Device ID string reported to the host (visible e.g. via
# lsusb/Windows "Details" tab). Adjust MFG/MDL to taste.
echo -n "MFG:Raspberry Pi;MDL:USB Printer;CLS:PRINTER;" \
    > "${GADGET}/functions/${FUNCTION}/pnp_string"
echo 10 > "${GADGET}/functions/${FUNCTION}/q_len"

ln -sf "${GADGET}/functions/${FUNCTION}" "${GADGET}/configs/c.1/"

# The printer function is fully kernel-driven (unlike a FunctionFS
# gadget backed by a userspace daemon), so it can be bound to the UDC
# immediately after the configfs tree is assembled.
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
if [ -z "${UDC}" ]; then
    echo "gadget_setup: no UDC found" >&2
    exit 1
fi

echo "${UDC}" > "${GADGET}/UDC"
echo "gadget_setup: bound to UDC '${UDC}'"
