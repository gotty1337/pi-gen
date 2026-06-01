#!/bin/bash
set -e

ROOTFS_DIR="${ROOTFS_DIR}"
FILES="$(dirname "$0")/files"
DEST="${ROOTFS_DIR}/home/pi/usb-gadget"

echo "Installing USB FunctionFS gadget to /home/pi/usb-gadget..."

install -d -m 755 "${DEST}"

install -m 644 "${FILES}/usb_gadget.c"          "${DEST}/usb_gadget.c"
install -m 755 "${FILES}/gadget_build.sh"       "${DEST}/gadget_build.sh"
install -m 755 "${FILES}/gadget_setup.sh"       "${DEST}/gadget_setup.sh"
install -m 755 "${FILES}/gadget_enable.sh"      "${DEST}/gadget_enable.sh"
install -m 755 "${FILES}/gadget_teardown.sh"    "${DEST}/gadget_teardown.sh"
install -m 644 "${FILES}/usb-gadget.service" \
    "${ROOTFS_DIR}/etc/systemd/system/usb-gadget.service"
install -m 644 "${FILES}/ipp-printer.service" \
    "${ROOTFS_DIR}/etc/systemd/system/ipp-printer.service"

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
chown -R pi:pi /home/pi/usb-gadget

# Create output directory for ippeveprinter print jobs
install -d -m 755 -o pi -g pi /home/pi/PDF

systemctl enable usb-gadget.service
systemctl enable ipp-printer.service
EOF
