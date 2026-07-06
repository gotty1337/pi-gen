#!/bin/bash
set -e

ROOTFS_DIR="${ROOTFS_DIR}"
FILES="$(dirname "$0")/files"
DEST="${ROOTFS_DIR}/home/pi/usb-raw-printer-gadget"

echo "Installing raw USB Printer gadget to /home/pi/usb-raw-printer-gadget..."

install -d -m 755 "${DEST}"

install -m 755 "${FILES}/gadget_setup.sh"    "${DEST}/gadget_setup.sh"
install -m 755 "${FILES}/gadget_teardown.sh" "${DEST}/gadget_teardown.sh"
install -m 644 "${FILES}/usb-raw-printer-gadget.service" \
    "${ROOTFS_DIR}/etc/systemd/system/usb-raw-printer-gadget.service"

# Configure the dwc2 USB controller for peripheral (device) mode.
# Remove any conflicting host or OTG-mode entries first, then append the
# peripheral overlay so it is always the last (and therefore effective) one.
CONFIG_TXT="${ROOTFS_DIR}/boot/firmware/config.txt"
sed -i '/^otg_mode=/d; /^dtoverlay=dwc2/d' "${CONFIG_TXT}"
echo "dtoverlay=dwc2,dr_mode=peripheral" >> "${CONFIG_TXT}"

# Ensure the dwc2 kernel module is loaded at boot via the cmdline.
CMDLINE="${ROOTFS_DIR}/boot/firmware/cmdline.txt"
if ! grep -q "modules-load=dwc2" "${CMDLINE}"; then
    sed -i 's/rootwait/rootwait modules-load=dwc2/' "${CMDLINE}"
fi

on_chroot << 'EOF'
chown -R pi:pi /home/pi/usb-raw-printer-gadget

# Not enabled: the gadget must be started manually, e.g.
#   sudo systemctl start usb-raw-printer-gadget.service
systemctl daemon-reload
EOF
