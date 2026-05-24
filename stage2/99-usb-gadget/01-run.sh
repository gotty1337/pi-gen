#!/bin/bash
set -e

ROOTFS_DIR="${ROOTFS_DIR}"
FILES="$(dirname "$0")/files"
DEST="${ROOTFS_DIR}/home/pi/usb-gadget"

echo "Installing USB FunctionFS gadget to /home/pi/usb-gadget..."

install -d -m 755 "${DEST}"

install -m 644 "${FILES}/usb_gadget.c"       "${DEST}/usb_gadget.c"
install -m 755 "${FILES}/gadget_build.sh"    "${DEST}/gadget_build.sh"
install -m 755 "${FILES}/gadget_setup.sh"    "${DEST}/gadget_setup.sh"
install -m 755 "${FILES}/gadget_enable.sh"   "${DEST}/gadget_enable.sh"
install -m 755 "${FILES}/gadget_teardown.sh" "${DEST}/gadget_teardown.sh"

install -m 644 "${FILES}/usb-gadget.service" \
    "${ROOTFS_DIR}/etc/systemd/system/usb-gadget.service"

on_chroot << 'EOF'
chown -R pi:pi /home/pi/usb-gadget
systemctl enable usb-gadget.service
EOF
