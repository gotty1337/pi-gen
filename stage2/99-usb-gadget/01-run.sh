#!/bin/bash
set -e

ROOTFS_DIR="${ROOTFS_DIR}"
FILES="$(dirname "$0")/files"

echo "Installing USB FunctionFS gadget..."

# Install C source so the service can compile it on first boot.
install -d -m 755 "${ROOTFS_DIR}/usr/local/src/usb-gadget"
install -m 644 "${FILES}/usb_gadget.c" \
    "${ROOTFS_DIR}/usr/local/src/usb-gadget/usb_gadget.c"

# Install helper scripts.
install -m 755 "${FILES}/gadget_build.sh"    "${ROOTFS_DIR}/usr/local/sbin/gadget_build.sh"
install -m 755 "${FILES}/gadget_setup.sh"    "${ROOTFS_DIR}/usr/local/sbin/gadget_setup.sh"
install -m 755 "${FILES}/gadget_enable.sh"   "${ROOTFS_DIR}/usr/local/sbin/gadget_enable.sh"
install -m 755 "${FILES}/gadget_teardown.sh" "${ROOTFS_DIR}/usr/local/sbin/gadget_teardown.sh"

# Install systemd service and enable it.
install -m 644 "${FILES}/usb-gadget.service" \
    "${ROOTFS_DIR}/etc/systemd/system/usb-gadget.service"

on_chroot << 'EOF'
systemctl enable usb-gadget.service
EOF
