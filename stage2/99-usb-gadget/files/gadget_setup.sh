#!/bin/bash
# gadget_setup.sh – Configure a USB FunctionFS bulk-loopback gadget via configfs.
#
# Call once before starting the usb_gadget daemon.  Idempotent: re-running
# while the gadget is already active is a no-op.
#
# Dependencies: libcomposite kernel module, dwc2 in peripheral mode (loaded by
# the boot overlay in config.txt / cmdline.txt).

set -euo pipefail

GADGET_NAME="g-loopback"
CONFIGFS="/sys/kernel/config"
GADGET="${CONFIGFS}/usb_gadget/${GADGET_NAME}"
FFS_DIR="/dev/ffs-loopback"

# --- load modules -------------------------------------------------------------
modprobe libcomposite

# --- mount configfs -----------------------------------------------------------
if ! mountpoint -q "${CONFIGFS}"; then
    mount -t configfs none "${CONFIGFS}"
fi

# --- skip if already configured -----------------------------------------------
if [ -d "${GADGET}" ]; then
    echo "gadget_setup: ${GADGET_NAME} already exists, skipping"
    exit 0
fi

# --- create gadget ------------------------------------------------------------
mkdir -p "${GADGET}"

echo 0x1d6b > "${GADGET}/idVendor"   # Linux Foundation
echo 0x0104 > "${GADGET}/idProduct"  # Multifunction Composite Gadget

mkdir -p "${GADGET}/strings/0x409"
echo "1234567890"            > "${GADGET}/strings/0x409/serialnumber"
echo "Raspberry Pi"          > "${GADGET}/strings/0x409/manufacturer"
echo "USB Bulk Loopback"     > "${GADGET}/strings/0x409/product"

# --- configuration ------------------------------------------------------------
mkdir -p "${GADGET}/configs/c.1"
echo 250 > "${GADGET}/configs/c.1/MaxPower"

mkdir -p "${GADGET}/configs/c.1/strings/0x409"
echo "Loopback config" > "${GADGET}/configs/c.1/strings/0x409/configuration"

# --- FunctionFS function ------------------------------------------------------
mkdir -p "${GADGET}/functions/ffs.usb0"

# Mount FFS so the daemon can write descriptors to ep0.
mkdir -p "${FFS_DIR}"
if ! mountpoint -q "${FFS_DIR}"; then
    mount -t functionfs usb0 "${FFS_DIR}"
fi

# Link function into the configuration.
ln -sf "${GADGET}/functions/ffs.usb0" "${GADGET}/configs/c.1/"

echo "gadget_setup: done – start usb_gadget daemon, then UDC will be enabled by gadget_enable.sh"
